/*-------------------------------------------------------------------------
 *
 * tuptoaster.c
 *	  Support routines for external and compressed storage of
 *	  variable size attributes.
 *
 * Copyright (c) 2000-2011, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/tuptoaster.c
 *
 *
 * INTERFACE ROUTINES
 *		toast_insert_or_update -
 *			Try to make a given tuple fit into one page by compressing
 *			or moving off attributes
 *
 *		toast_delete -
 *			Reclaim toast storage when a tuple is deleted
 *
 *		heap_tuple_untoast_attr -
 *			Fetch back a given value from the "secondary" relation
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "utils/fmgroids.h"
#include "utils/pg_lzcompress.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/tqual.h"

/* GPDB additions */
#include "utils/guc.h"

#undef TOAST_DEBUG

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */
#define TOAST_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(struct varatt_external))

/*
 * Testing whether an externally-stored value is compressed now requires
 * comparing extsize (the actual length of the external data) to rawsize
 * (the original uncompressed datum's size).  The latter includes VARHDRSZ
 * overhead, the former doesn't.  We never use compression unless it actually
 * saves space, so we expect either equality or less-than.
 */
#define VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) \
	((toast_pointer).va_extsize < (toast_pointer).va_rawsize - VARHDRSZ)

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "struct varatt_external" toast pointer.  This should be
 * just a memcpy, but some versions of gcc seem to produce broken code
 * that assumes the datum contents are aligned.  Introducing an explicit
 * intermediate "varattrib_1b_e *" variable seems to fix it.
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)


#define SET_VARSIZE_C(PTR)			(((varattrib_1b *) (PTR))->va_header |= 0x40)

static void toast_delete_datum(Relation rel, Datum value);
static Datum toast_save_datum(Relation rel, Datum value, bool isFrozen, int options);
static struct varlena *toast_fetch_datum(struct varlena * attr);
static struct varlena *toast_fetch_datum_slice(struct varlena * attr,
						int32 sliceoffset, int32 length);


/*
 * GPDB: Check to be sure that a toast table's index is valid before making use
 * of it in a query.
 *
 * GPDB_94_MERGE_FIXME: This function exists only because we don't yet have
 * upstream commit 2ef085d0e. Once that's merged, we can get rid of this.
 */
static void
check_toast_indisvalid(Relation toastrel, Relation toastidx)
{
	Assert(RelationIsValid(toastidx));
	Assert(RelationIsValid(toastrel));
	Assert(PointerIsValid(toastidx->rd_index));

	if (!IndexIsValid(toastidx->rd_index))
		elog(ERROR, "no valid index found for toast relation with Oid %d",
			 RelationGetRelid(toastrel));
}


/* ----------
 * heap_tuple_fetch_attr -
 *
 *	Public entry point to get back a toasted value from
 *	external storage (possibly still in compressed format).
 *
 * This will return a datum that contains all the data internally, ie, not
 * relying on external storage, but it can still be compressed or have a short
 * header.
 ----------
 */
struct varlena *
heap_tuple_fetch_attr(struct varlena * attr)
{
	struct varlena *result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		/*
		 * This is an external stored plain value
		 */
		result = toast_fetch_datum(attr);
	}
	else
	{
		/*
		 * This is a plain value inside of the main tuple - why am I called?
		 */
		result = attr;
	}

	return result;
}


/*
 * If this function is changed then update varattrib_untoast_ptr_len as well
 */
int
varattrib_untoast_len(Datum d)
{
	if (DatumGetPointer(d) == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg(" Unable to detoast datum "),
				 errprintstack(true)));
	}

	struct varlena *attr = (struct varlena *) DatumGetPointer(d);
	struct varlena *result;

	int len = -1;
	void *toFree = NULL;

	if (VARATT_IS_EXTENDED(attr))
	{
		if (VARATT_IS_EXTERNAL(attr))
		{
			result = toast_fetch_datum(attr);
			/* toast_fetch_datum will palloc, so set it up for free */
			toFree = result;
		}

		if (VARATT_IS_COMPRESSED(attr))
		{
			PGLZ_Header *tmp = (PGLZ_Header *) attr;
			len = PGLZ_RAW_SIZE(tmp);
		}
		else if (VARATT_IS_SHORT(attr))
		{
			len = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
		}
	}

	if(len == -1)
		len = VARSIZE(attr) - VARHDRSZ;

	if (toFree)
		pfree(toFree);

	Assert(len >= 0);
	return len;
}

/*
 * If this function is changed then update varattrib_untoast_len as well
 */
void
varattrib_untoast_ptr_len(Datum d, char **datastart, int *len, void **tofree)
{
	if (DatumGetPointer(d) == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg(" Unable to detoast datum "),
				 errprintstack(true)));
	}

	struct varlena *va = (struct varlena *) DatumGetPointer(d);
	struct varlena *attr = va;

	*len = -1;
	*tofree = NULL;

	if (VARATT_IS_EXTENDED(va))
	{
		if (VARATT_IS_EXTERNAL(va))
		{
			attr = toast_fetch_datum(va);
			/* toast_fetch_datum will palloc, so set it up for free */
			*tofree = attr;
		}

		if (VARATT_IS_COMPRESSED(attr))
		{
			PGLZ_Header *tmp = (PGLZ_Header *) attr;
			attr = (struct varlena *) palloc(PGLZ_RAW_SIZE(tmp) + VARHDRSZ);
			SET_VARSIZE(attr, PGLZ_RAW_SIZE(tmp) + VARHDRSZ);
			pglz_decompress(tmp, VARDATA(attr));

			/* If tofree is set, that is, we get it from toast_fetch_datum.  
			 * We need to free it here 
			 */
			if(*tofree)
				pfree(*tofree);
			*tofree = attr;
		}
		else if (VARATT_IS_SHORT(attr))
		{
		    /* Warning! Return unaligned pointer! */
			*len = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
			*datastart = VARDATA_SHORT(attr);
			attr = NULL;
		}
	}

	if (*len == -1)
	{
		*datastart = VARDATA(attr);
		*len = VARSIZE(attr) - VARHDRSZ;
	}

	Assert(*len >= 0);
}

/* ----------
 * heap_tuple_untoast_attr -
 *
 *	Public entry point to get back a toasted value from compression
 *	or external storage.
 * ----------
 */
struct varlena *
heap_tuple_untoast_attr(struct varlena * attr)
{
	if (VARATT_IS_EXTERNAL(attr))
	{
		/*
		 * This is an externally stored datum --- fetch it back from there
		 */
		attr = toast_fetch_datum(attr);
		/* If it's compressed, decompress it */
		if (VARATT_IS_COMPRESSED(attr))
		{
			PGLZ_Header *tmp = (PGLZ_Header *) attr;

			attr = (struct varlena *) palloc(PGLZ_RAW_SIZE(tmp) + VARHDRSZ);
			SET_VARSIZE(attr, PGLZ_RAW_SIZE(tmp) + VARHDRSZ);
			pglz_decompress(tmp, VARDATA(attr));
			pfree(tmp);
		}
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/*
		 * This is a compressed value inside of the main tuple
		 */
		PGLZ_Header *tmp = (PGLZ_Header *) attr;

		attr = (struct varlena *) palloc(PGLZ_RAW_SIZE(tmp) + VARHDRSZ);
		SET_VARSIZE(attr, PGLZ_RAW_SIZE(tmp) + VARHDRSZ);
		pglz_decompress(tmp, VARDATA(attr));
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * This is a short-header varlena --- convert to 4-byte header format
		 */
		Size		data_size = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
		Size		new_size = data_size + VARHDRSZ;
		struct varlena *new_attr;

		new_attr = (struct varlena *) palloc(new_size);
		SET_VARSIZE(new_attr, new_size);
		memcpy(VARDATA(new_attr), VARDATA_SHORT(attr), data_size);
		attr = new_attr;
	}

	return attr;
}


/* ----------
 * heap_tuple_untoast_attr_slice -
 *
 *		Public entry point to get back part of a toasted value
 *		from compression or external storage.
 * ----------
 */
struct varlena *
heap_tuple_untoast_attr_slice(struct varlena * attr,
							  int32 sliceoffset, int32 slicelength)
{
	struct varlena *preslice;
	struct varlena *result;
	char	   *attrdata;
	int32		attrsize;

	if (VARATT_IS_EXTERNAL(attr))
	{
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* fast path for non-compressed external datums */
		if (!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			return toast_fetch_datum_slice(attr, sliceoffset, slicelength);

		/* fetch it back (compressed marker will get set automatically) */
		preslice = toast_fetch_datum(attr);
	}
	else
		preslice = attr;

	if (VARATT_IS_COMPRESSED(preslice))
	{
		PGLZ_Header *tmp = (PGLZ_Header *) preslice;
		Size		size = PGLZ_RAW_SIZE(tmp) + VARHDRSZ;

		preslice = (struct varlena *) palloc(size);
		SET_VARSIZE(preslice, size);
		pglz_decompress(tmp, VARDATA(preslice));

		if (tmp != (PGLZ_Header *) attr)
			pfree(tmp);
	}

	if (VARATT_IS_SHORT(preslice))
	{
		attrdata = VARDATA_SHORT(preslice);
		attrsize = VARSIZE_SHORT(preslice) - VARHDRSZ_SHORT;
	}
	else
	{
		attrdata = VARDATA(preslice);
		attrsize = VARSIZE(preslice) - VARHDRSZ;
	}

	/* slicing of datum for compressed cases and plain value */

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}

	if (((sliceoffset + slicelength) > attrsize) || slicelength < 0)
		slicelength = attrsize - sliceoffset;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, slicelength + VARHDRSZ);

	memcpy(VARDATA(result), attrdata + sliceoffset, slicelength);

	if (preslice != attr)
		pfree(preslice);

	return result;
}


/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 *	(including the VARHDRSZ header)
 * ----------
 */
Size
toast_raw_datum_size(Datum value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		/* va_rawsize is the size of the original datum -- including header */
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = toast_pointer.va_rawsize;
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/* here, va_rawsize is just the payload size */
		result = VARRAWSIZE_4B_C(attr) + VARHDRSZ;
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * we have to normalize the header length to VARHDRSZ or else the
		 * callers of this function will be confused.
		 */
		result = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT + VARHDRSZ;
	}
	else
	{
		/* plain untoasted datum */
		result = VARSIZE(attr);
	}
	return result;
}

/* ----------
 * toast_datum_size
 *
 *	Return the physical storage size (possibly compressed) of a varlena datum
 * ----------
 */
Size
toast_datum_size(Datum value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		/*
		 * Attribute is stored externally - return the extsize whether
		 * compressed or not.  We do not count the size of the toast pointer
		 * ... should we?
		 */
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = toast_pointer.va_extsize;
	}
	else if (VARATT_IS_SHORT(attr))
	{
		result = VARSIZE_SHORT(attr);
	}
	else
	{
		/*
		 * Attribute is stored inline either compressed or not, just calculate
		 * the size of the datum in either case.
		 */
		result = VARSIZE(attr);
	}
	return result;
}


/* ----------
 * toast_delete -
 *
 *	Cascaded delete toast-entries on DELETE
 * ----------
 */
void
toast_delete(Relation rel, GenericTuple oldtup, MemTupleBinding *pbind)
{
	TupleDesc	tupleDesc;
	Form_pg_attribute *att;
	int			numAttrs;
	int			i;
	Datum		toast_values[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];
	bool 		ismemtuple = is_memtuple(oldtup);
	
	AssertImply(ismemtuple, pbind);
	AssertImply(!ismemtuple, !pbind);

	/*
	 * We should only ever be called for tuples of plain relations ---
	 * recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	/*
	 * We should only ever be called for tuples of plain relations ---
	 * recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	/*
	 * Get the tuple descriptor and break down the tuple into fields.
	 *
	 * NOTE: it's debatable whether to use heap_deform_tuple() here or just
	 * heap_getattr() only the varlena columns.  The latter could win if there
	 * are few varlena columns and many non-varlena ones. However,
	 * heap_deform_tuple costs only O(N) while the heap_getattr way would cost
	 * O(N^2) if there are many varlena columns, so it seems better to err on
	 * the side of linear cost.  (We won't even be here unless there's at
	 * least one varlena column, by the way.)
	 */
	tupleDesc = rel->rd_att;
	att = tupleDesc->attrs;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);

	if (ismemtuple)
		memtuple_deform((MemTuple) oldtup, pbind, toast_values, toast_isnull);
	else
		heap_deform_tuple((HeapTuple) oldtup, tupleDesc, toast_values, toast_isnull);

	/*
	 * Check for external stored attributes and delete them from the secondary
	 * relation.
	 */
	for (i = 0; i < numAttrs; i++)
	{
		if (att[i]->attlen == -1)
		{
			Datum		value = toast_values[i];

			if (!toast_isnull[i] && VARATT_IS_EXTERNAL(PointerGetDatum(value)))
				toast_delete_datum(rel, value);
		}
	}
}


/* ----------
 * toast_insert_or_update -
 *
 *	Delete no-longer-used toast-entries and create new ones to
 *	make the new tuple fit on INSERT or UPDATE
 *
 * Inputs:
 *	newtup: the candidate new tuple to be inserted
 *	oldtup: the old row version for UPDATE, or NULL for INSERT
 *	options: options to be passed to heap_insert() for toast rows
 * Result:
 *	either newtup if no toasting is needed, or a palloc'd modified tuple
 *	that is what should actually get stored
 *
 * NOTE: neither newtup nor oldtup will be modified.  This is a change
 * from the pre-8.1 API of this routine.
 * ----------
 */
static int
compute_dest_tuplen(TupleDesc tupdesc, MemTupleBinding *pbind, bool hasnull, Datum *d, bool *isnull)
{
	if(pbind) 
	{
		uint32 nullsave_dummy;
		return (int) compute_memtuple_size(pbind, d, isnull, hasnull, &nullsave_dummy);
	}

	return heap_compute_data_size(tupdesc, d, isnull);
}


static GenericTuple
toast_insert_or_update_generic(Relation rel, GenericTuple newtup, GenericTuple oldtup,
					   MemTupleBinding *pbind, int toast_tuple_target,
					   bool isFrozen, int options)
{
	GenericTuple result_gtuple;
	TupleDesc	tupleDesc;
	Form_pg_attribute *att;
	int			numAttrs;
	int			i;

	bool		need_change = false;
	bool		need_free = false;
	bool		need_delold = false;
	bool		has_nulls = false;

	Size		maxDataLen;
	Size		hoff;

	char		toast_action[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	int32		toast_sizes[MaxHeapAttributeNumber];
	bool		toast_free[MaxHeapAttributeNumber];
	bool		toast_delold[MaxHeapAttributeNumber];

	bool 		ismemtuple = is_memtuple(newtup);

	AssertImply(ismemtuple, pbind);
	AssertImply(!ismemtuple, !pbind);
	AssertImply(ismemtuple && oldtup, is_memtuple(oldtup));
	Assert(toast_tuple_target > 0);

	/*
	 * We should only ever be called for tuples of plain relations ---
	 * recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	/*
	 * Get the tuple descriptor and break down the tuple(s) into fields.
	 */
	tupleDesc = rel->rd_att;
	att = tupleDesc->attrs;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);

	if(ismemtuple)
		memtuple_deform((MemTuple) newtup, pbind, toast_values, toast_isnull);
	else
		heap_deform_tuple((HeapTuple) newtup, tupleDesc, toast_values, toast_isnull);

	if (oldtup != NULL)
	{
		if(ismemtuple)
			memtuple_deform((MemTuple) oldtup, pbind, toast_oldvalues, toast_oldisnull);
		else
			heap_deform_tuple((HeapTuple) oldtup, tupleDesc, toast_oldvalues, toast_oldisnull);
	}
	/* ----------
	 * Then collect information about the values given
	 *
	 * NOTE: toast_action[i] can have these values:
	 *		' '		default handling
	 *		'p'		already processed --- don't touch it
	 *		'x'		incompressible, but OK to move off
	 *
	 * NOTE: toast_sizes[i] is only made valid for varlena attributes with
	 *		toast_action[i] different from 'p'.
	 * ----------
	 */
	memset(toast_action, ' ', numAttrs * sizeof(char));
	memset(toast_free, 0, numAttrs * sizeof(bool));
	memset(toast_delold, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		struct varlena *old_value;
		struct varlena *new_value;

		if (oldtup != NULL)
		{
			/*
			 * For UPDATE get the old and new values of this attribute
			 */
			old_value = (struct varlena *) DatumGetPointer(toast_oldvalues[i]);
			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);

			/*
			 * If the old value is an external stored one, check if it has
			 * changed so we have to delete it later.
			 */
			if (att[i]->attlen == -1 && !toast_oldisnull[i] &&
				VARATT_IS_EXTERNAL(old_value))
			{
				if (toast_isnull[i] || !VARATT_IS_EXTERNAL(new_value) ||
					memcmp((char *) old_value, (char *) new_value,
						   VARSIZE_EXTERNAL(old_value)) != 0)
				{
					/*
					 * The old external stored value isn't needed any more
					 * after the update
					 */
					toast_delold[i] = true;
					need_delold = true;
				}
				else
				{
					/*
					 * This attribute isn't changed by this update so we reuse
					 * the original reference to the old value in the new
					 * tuple.
					 */
					toast_action[i] = 'p';
					continue;
				}
			}
		}
		else
		{
			/*
			 * For INSERT simply get the new value
			 */
			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
		}

		/*
		 * Handle NULL attributes
		 */
		if (toast_isnull[i])
		{
			toast_action[i] = 'p';
			has_nulls = true;
			continue;
		}

		/*
		 * Now look at varlena attributes
		 */
		if (att[i]->attlen == -1)
		{
			/*
			 * If the table's attribute says PLAIN always, force it so.
			 */
			if (att[i]->attstorage == 'p')
				toast_action[i] = 'p';

			/*
			 * We took care of UPDATE above, so any external value we find
			 * still in the tuple must be someone else's we cannot reuse.
			 * Fetch it back (without decompression, unless we are forcing
			 * PLAIN storage).	If necessary, we'll push it out as a new
			 * external value below.
			 */
			if (VARATT_IS_EXTERNAL(new_value))
			{
				if (att[i]->attstorage == 'p')
					new_value = heap_tuple_untoast_attr(new_value);
				else
					new_value = heap_tuple_fetch_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
				need_change = true;
				need_free = true;
			}

			/*
			 * Remember the size of this attribute
			 */
			toast_sizes[i] = VARSIZE_ANY(new_value);
		}
		else
		{
			/*
			 * Not a varlena attribute, plain storage always
			 */
			toast_action[i] = 'p';
		}
	}

	/* ----------
	 * Compress and/or save external until data fits into target length
	 *
	 *	1: Inline compress attributes with attstorage 'x', and store very
	 *	   large attributes with attstorage 'x' or 'e' external immediately
	 *	2: Store attributes with attstorage 'x' or 'e' external
	 *	3: Inline compress attributes with attstorage 'm'
	 *	4: Store attributes with attstorage 'm' external
	 * ----------
	 */

	if (!ismemtuple)
	{
		/* compute header overhead --- this should match heap_form_tuple() */
		hoff = offsetof(HeapTupleHeaderData, t_bits);
		if (has_nulls)
			hoff += BITMAPLEN(numAttrs);
		if (((HeapTuple) newtup)->t_data->t_infomask & HEAP_HASOID)
			hoff += sizeof(Oid);
		hoff = MAXALIGN(hoff);
		/* now convert to a limit on the tuple data size */
		maxDataLen = TOAST_TUPLE_TARGET - hoff;
	}
	else
	{
		maxDataLen = toast_tuple_target;
		hoff = -1; /* keep compiler quiet about using 'hoff' uninitialized */
	}

	/*
	 * Look for attributes with attstorage 'x' to compress.  Also find large
	 * attributes with attstorage 'x' or 'e', and store them external.
	 */
	while (compute_dest_tuplen(tupleDesc, pbind, has_nulls,
							   toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum		old_value;
		Datum		new_value;

		/*
		 * Search for the biggest yet unprocessed internal attribute
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
			if (VARATT_IS_COMPRESSED(DatumGetPointer(toast_values[i])))
				continue;
			if (att[i]->attstorage != 'x' && att[i]->attstorage != 'e')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Attempt to compress it inline, if it has attstorage 'x'
		 */
		i = biggest_attno;
		if (att[i]->attstorage == 'x')
		{
			old_value = toast_values[i];
			new_value = toast_compress_datum(old_value);

			if (DatumGetPointer(new_value) != NULL)
			{
				/* successful compression */
				if (toast_free[i])
					pfree(DatumGetPointer(old_value));
				toast_values[i] = new_value;
				toast_free[i] = true;
				toast_sizes[i] = VARSIZE(DatumGetPointer(toast_values[i]));
				need_change = true;
				need_free = true;
			}
			else
			{
				/* incompressible, ignore on subsequent compression passes */
				toast_action[i] = 'x';
			}
		}
		else
		{
			/* has attstorage 'e', ignore on subsequent compression passes */
			toast_action[i] = 'x';
		}

		/*
		 * If this value is by itself more than maxDataLen (after compression
		 * if any), push it out to the toast table immediately, if possible.
		 * This avoids uselessly compressing other fields in the common case
		 * where we have one long field and several short ones.
		 *
		 * XXX maybe the threshold should be less than maxDataLen?
		 */
		if (toast_sizes[i] > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
		{
			old_value = toast_values[i];
			toast_action[i] = 'p';
			toast_values[i] = toast_save_datum(rel, toast_values[i], isFrozen, options);
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_free[i] = true;
			need_change = true;
			need_free = true;
		}
	}

	/*
	 * Second we look for attributes of attstorage 'x' or 'e' that are still
	 * inline.	But skip this if there's no toast table to push them to.
	 */
	while (compute_dest_tuplen(tupleDesc, pbind, has_nulls,
							   toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum		old_value;

		/*------
		 * Search for the biggest yet inlined attribute with
		 * attstorage equals 'x' or 'e'
		 *------
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
			if (att[i]->attstorage != 'x' && att[i]->attstorage != 'e')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Store this external
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		toast_action[i] = 'p';
		toast_values[i] = toast_save_datum(rel, toast_values[i], isFrozen, options);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));
		toast_free[i] = true;

		need_change = true;
		need_free = true;
	}

	/*
	 * Round 3 - this time we take attributes with storage 'm' into
	 * compression
	 */
	while (compute_dest_tuplen(tupleDesc, pbind, has_nulls,
							   toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum		old_value;
		Datum		new_value;

		/*
		 * Search for the biggest yet uncompressed internal attribute
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
			if (VARATT_IS_COMPRESSED(DatumGetPointer(toast_values[i])))
				continue;
			if (att[i]->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Attempt to compress it inline
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		new_value = toast_compress_datum(old_value);

		if (DatumGetPointer(new_value) != NULL)
		{
			/* successful compression */
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_values[i] = new_value;
			toast_free[i] = true;
			toast_sizes[i] = VARSIZE(DatumGetPointer(toast_values[i]));
			need_change = true;
			need_free = true;
		}
		else
		{
			/* incompressible, ignore on subsequent compression passes */
			toast_action[i] = 'x';
		}
	}

	/*
	 * Finally we store attributes of type 'm' externally.	At this point we
	 * increase the target tuple size, so that 'm' attributes aren't stored
	 * externally unless really necessary.
	 */
	/*
	 * FIXME: Should we do something like this with memtuples on
	 * AO tables too? Currently we do not increase the target tuple size for AO
	 * table, so there are occasions when columns of type 'm' will be stored
	 * out-of-line but they could otherwise be accommodated in-block
	 * c.f. upstream Postgres commit ca7c8168de76459380577eda56a3ed09b4f6195c
	 */
	if (!ismemtuple)
		maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;

	while (compute_dest_tuplen(tupleDesc, pbind, has_nulls,
							   toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum		old_value;

		/*--------
		 * Search for the biggest yet inlined attribute with
		 * attstorage = 'm'
		 *--------
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
			if (att[i]->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Store this external
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		toast_action[i] = 'p';
		toast_values[i] = toast_save_datum(rel, toast_values[i], isFrozen, options);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));
		toast_free[i] = true;

		need_change = true;
		need_free = true;
	}

	/* XXX Maybe we should check here for any compressed inline attributes that
	 * didn't save enough to warrant keeping. In particular attributes whose
	 * rawsize is < 128 bytes and didn't save at least 3 bytes... or even maybe
	 * more given alignment issues 
	 */

	/*
	 * In the case we toasted any values, we need to build a new heap tuple
	 * with the changed values.
	 */
	if (need_change)
	{
		if(ismemtuple)
		{
			result_gtuple = (GenericTuple) memtuple_form_to(pbind, toast_values, toast_isnull, NULL, NULL, false);
			if (mtbind_has_oid(pbind))
			{
				Oid			oid;

				oid = MemTupleGetOid((MemTuple) newtup, pbind);
				MemTupleSetOid((MemTuple) result_gtuple, pbind, oid);
			}
		}
		else
		{
			HeapTupleHeader olddata = ((HeapTuple) newtup)->t_data;
			HeapTupleHeader new_data;
			int32		new_header_len;
			int32		new_data_len;
			int32		new_tuple_len;
			HeapTuple	result_tuple;

			/*
			 * Calculate the new size of the tuple.
			 *
			 * Note: we used to assume here that the old tuple's t_hoff must equal
			 * the new_header_len value, but that was incorrect.  The old tuple
			 * might have a smaller-than-current natts, if there's been an ALTER
			 * TABLE ADD COLUMN since it was stored; and that would lead to a
			 * different conclusion about the size of the null bitmap, or even
			 * whether there needs to be one at all.
			 */
			new_header_len = offsetof(HeapTupleHeaderData, t_bits);
			if (has_nulls)
				new_header_len += BITMAPLEN(numAttrs);
			if (olddata->t_infomask & HEAP_HASOID)
				new_header_len += sizeof(Oid);
			new_header_len = MAXALIGN(new_header_len);
			new_data_len = heap_compute_data_size(tupleDesc,
												  toast_values, toast_isnull);
			new_tuple_len = new_header_len + new_data_len;

			/*
			 * Allocate and zero the space needed, and fill HeapTupleData fields.
			 */
			result_tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + new_tuple_len);
			result_tuple->t_len = new_tuple_len;
			result_tuple->t_self = ((HeapTuple) newtup)->t_self;
			new_data = (HeapTupleHeader) ((char *) result_tuple + HEAPTUPLESIZE);
			result_tuple->t_data = new_data;

			/*
			 * Copy the existing tuple header, but adjust natts and t_hoff.
			 */
			memcpy(new_data, olddata, offsetof(HeapTupleHeaderData, t_bits));
			HeapTupleHeaderSetNatts(new_data, numAttrs);
			new_data->t_hoff = new_header_len;
			if (olddata->t_infomask & HEAP_HASOID)
				HeapTupleHeaderSetOid(new_data, HeapTupleHeaderGetOid(olddata));

			/* Copy over the data, and fill the null bitmap if needed */
			heap_fill_tuple(tupleDesc,
							toast_values,
							toast_isnull,
							(char *) new_data + new_header_len,
							new_data_len,
							&(new_data->t_infomask),
							has_nulls ? new_data->t_bits : NULL);
			result_gtuple = (GenericTuple) result_tuple;
		}
	}
	else
		result_gtuple = newtup;

	/*
	 * Free allocated temp values
	 */
	if (need_free)
		for (i = 0; i < numAttrs; i++)
			if (toast_free[i])
				pfree(DatumGetPointer(toast_values[i]));

	/*
	 * Delete external values from the old tuple
	 */
	if (need_delold)
		for (i = 0; i < numAttrs; i++)
			if (toast_delold[i])
				toast_delete_datum(rel, toast_oldvalues[i]);

	return result_gtuple;
}

HeapTuple
toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup,
					   int toast_tuple_target,
					   bool isFrozen, int options)
{
	return (HeapTuple) toast_insert_or_update_generic(rel,
													  (GenericTuple) newtup,
													  (GenericTuple) oldtup,
													  NULL,
													  toast_tuple_target,
													  isFrozen,
													  options);
}

MemTuple
toast_insert_or_update_memtup(Relation rel, MemTuple newtup, MemTuple oldtup,
					   MemTupleBinding *pbind, int toast_tuple_target,
					   bool isFrozen, int options)
{
	return (MemTuple) toast_insert_or_update_generic(rel,
													 (GenericTuple) newtup,
													 (GenericTuple) oldtup,
													 pbind,
													 toast_tuple_target,
													 isFrozen,
													 options);
}

/* ----------
 * toast_flatten_tuple -
 *
 *	"Flatten" a tuple to contain no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 * ----------
 */
HeapTuple
toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc)
{
	HeapTuple	new_tuple;
	Form_pg_attribute *att = tupleDesc->attrs;
	int			numAttrs = tupleDesc->natts;
	int			i;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * Break down the tuple into fields.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(tup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (!toast_isnull[i] && att[i]->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = toast_fetch_datum(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * Form the reconfigured tuple.
	 */
	new_tuple = heap_form_tuple(tupleDesc, toast_values, toast_isnull);

	/*
	 * Be sure to copy the tuple's OID and identity fields.  We also make a
	 * point of copying visibility info, just in case anybody looks at those
	 * fields in a syscache entry.
	 */
	if (tupleDesc->tdhasoid)
		HeapTupleSetOid(new_tuple, HeapTupleGetOid(tup));

	new_tuple->t_self = tup->t_self;

	new_tuple->t_data->t_choice = tup->t_data->t_choice;
	new_tuple->t_data->t_ctid = tup->t_data->t_ctid;
	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		tup->t_data->t_infomask & HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask2 |=
		tup->t_data->t_infomask2 & HEAP2_XACT_MASK;

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return new_tuple;
}


/* ----------
 * toast_flatten_tuple_attribute -
 *
 *	If a Datum is of composite type, "flatten" it to contain no toasted fields.
 *	This must be invoked on any potentially-composite field that is to be
 *	inserted into a tuple.	Doing this preserves the invariant that toasting
 *	goes only one level deep in a tuple.
 *
 *	Note that flattening does not mean expansion of short-header varlenas,
 *	so in one sense toasting is allowed within composite datums.
 * ----------
 */
Datum
toast_flatten_tuple_attribute(Datum value,
							  Oid typeId, int32 typeMod)
{
	TupleDesc	tupleDesc;
	HeapTupleHeader olddata;
	HeapTupleHeader new_data;
	int32		new_header_len;
	int32		new_data_len;
	int32		new_tuple_len;
	HeapTupleData tmptup;
	Form_pg_attribute *att;
	int			numAttrs;
	int			i;
	bool		need_change = false;
	bool		has_nulls = false;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * See if it's a composite type, and get the tupdesc if so.
	 */
	tupleDesc = lookup_rowtype_tupdesc_noerror(typeId, typeMod, true);
	if (tupleDesc == NULL)
		return value;			/* not a composite type */

	att = tupleDesc->attrs;
	numAttrs = tupleDesc->natts;

	/*
	 * Break down the tuple into fields.
	 */
	olddata = DatumGetHeapTupleHeader(value);
	Assert(typeId == HeapTupleHeaderGetTypeId(olddata));
	Assert(typeMod == HeapTupleHeaderGetTypMod(olddata));
	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(olddata);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_data = olddata;

	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(&tmptup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (toast_isnull[i])
			has_nulls = true;
		else if (att[i]->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value) ||
				VARATT_IS_COMPRESSED(new_value))
			{
				new_value = heap_tuple_untoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
				need_change = true;
			}
		}
	}

	/*
	 * If nothing to untoast, just return the original tuple.
	 */
	if (!need_change)
	{
		ReleaseTupleDesc(tupleDesc);
		return value;
	}

	/*
	 * Calculate the new size of the tuple.
	 *
	 * This should match the reconstruction code in toast_insert_or_update.
	 */
	new_header_len = offsetof(HeapTupleHeaderData, t_bits);
	if (has_nulls)
		new_header_len += BITMAPLEN(numAttrs);
	if (olddata->t_infomask & HEAP_HASOID)
		new_header_len += sizeof(Oid);
	new_header_len = MAXALIGN(new_header_len);
	new_data_len = heap_compute_data_size(tupleDesc,
										  toast_values, toast_isnull);
	new_tuple_len = new_header_len + new_data_len;

	new_data = (HeapTupleHeader) palloc0(new_tuple_len);

	/*
	 * Copy the existing tuple header, but adjust natts and t_hoff.
	 */
	memcpy(new_data, olddata, offsetof(HeapTupleHeaderData, t_bits));
	HeapTupleHeaderSetNatts(new_data, numAttrs);
	new_data->t_hoff = new_header_len;
	if (olddata->t_infomask & HEAP_HASOID)
		HeapTupleHeaderSetOid(new_data, HeapTupleHeaderGetOid(olddata));

	/* Reset the datum length field, too */
	HeapTupleHeaderSetDatumLength(new_data, new_tuple_len);

	/* Copy over the data, and fill the null bitmap if needed */
	heap_fill_tuple(tupleDesc,
					toast_values,
					toast_isnull,
					(char *) new_data + new_header_len,
					new_data_len,
					&(new_data->t_infomask),
					has_nulls ? new_data->t_bits : NULL);

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));
	ReleaseTupleDesc(tupleDesc);

	return PointerGetDatum(new_data);
}


/* ----------
 * toast_compress_datum -
 *
 *	Create a compressed version of a varlena datum
 *
 *	If we fail (ie, compressed result is actually bigger than original)
 *	then return NULL.  We must not use compressed data if it'd expand
 *	the tuple!
 *
 *	We use VAR{SIZE,DATA}_ANY so we can handle short varlenas here without
 *	copying them.  But we can't handle external or compressed datums.
 * ----------
 */
Datum
toast_compress_datum(Datum value)
{
	struct varlena *tmp;
	int32		valsize = VARSIZE_ANY_EXHDR(DatumGetPointer(value));

	Assert(!VARATT_IS_EXTERNAL(DatumGetPointer(value)));
	Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));

	/*
	 * No point in wasting a palloc cycle if value size is out of the allowed
	 * range for compression
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return PointerGetDatum(NULL);

	tmp = (struct varlena *) palloc(PGLZ_MAX_OUTPUT(valsize));

	/*
	 * We recheck the actual size even if pglz_compress() reports success,
	 * because it might be satisfied with having saved as little as one byte
	 * in the compressed data --- which could turn into a net loss once you
	 * consider header and alignment padding.  Worst case, the compressed
	 * format might require three padding bytes (plus header, which is
	 * included in VARSIZE(tmp)), whereas the uncompressed format would take
	 * only one header byte and no padding if the value is short enough.  So
	 * we insist on a savings of more than 2 bytes to ensure we have a gain.
	 */
	if (pglz_compress(VARDATA_ANY(DatumGetPointer(value)), valsize,
					  (PGLZ_Header *) tmp, PGLZ_strategy_default) &&
		VARSIZE(tmp) < valsize - 2)
	{
		/* successful compression */
		return PointerGetDatum(tmp);
	}
	else
	{
		/* incompressible data */
		pfree(tmp);
		return PointerGetDatum(NULL);
	}
}


/* ----------
 * toast_save_datum -
 *
 *	Save one single datum into the secondary relation and return
 *	a Datum reference for it.
 * ----------
 */
static Datum
toast_save_datum(Relation rel, Datum value, bool isFrozen, int options)
{
	Relation	toastrel;
	Relation	toastidx;
	HeapTuple	toasttup;
	TupleDesc	toasttupDesc;
	Datum		t_values[3];
	bool		t_isnull[3];
	TransactionId myxid = GetCurrentTransactionId();
	CommandId	mycid = GetCurrentCommandId(true);
	struct varlena *result;
	struct varatt_external toast_pointer;
	struct
	{
		struct varlena hdr;
		char		data[TOAST_MAX_CHUNK_SIZE]; /* make struct big enough */
		int32		align_it;	/* ensure struct is aligned well enough */
	}			chunk_data;
	int32		chunk_size;
	int32		chunk_seq = 0;
	char	   *data_p;
	int32		data_todo;
	Pointer		dval = DatumGetPointer(value);

	int32		max_chunk_size;

	/*
	 * Open the toast relation and its index.  We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	toastrel = heap_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(toastrel->rd_rel->reltoastidxid, RowExclusiveLock);

	check_toast_indisvalid(toastrel, toastidx);

	/*
	 * Get the data pointer and length, and compute va_rawsize and va_extsize.
	 *
	 * va_rawsize is the size of the equivalent fully uncompressed datum, so
	 * we have to adjust for short headers.
	 *
	 * va_extsize is the actual size of the data payload in the toast records.
	 */
	if (VARATT_IS_SHORT(dval))
	{
		data_p = VARDATA_SHORT(dval);
		data_todo = VARSIZE_SHORT(dval) - VARHDRSZ_SHORT;
		toast_pointer.va_rawsize = data_todo + VARHDRSZ;		/* as if not short */
		toast_pointer.va_extsize = data_todo;
	}
	else if (VARATT_IS_COMPRESSED(dval))
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		/* rawsize in a compressed datum is just the size of the payload */
		toast_pointer.va_rawsize = VARRAWSIZE_4B_C(dval) + VARHDRSZ;
		toast_pointer.va_extsize = data_todo;
		/* Assert that the numbers look like it's compressed */
		Assert(VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
	}
	else
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		toast_pointer.va_rawsize = VARSIZE(dval);
		toast_pointer.va_extsize = data_todo;
	}

	/*
	 * Insert the correct table OID into the result TOAST pointer.
	 *
	 * Normally this is the actual OID of the target toast table, but during
	 * table-rewriting operations such as CLUSTER, we have to insert the OID
	 * of the table's real permanent toast table instead.  rd_toastoid is set
	 * if we have to substitute such an OID.
	 */
	if (OidIsValid(rel->rd_toastoid))
		toast_pointer.va_toastrelid = rel->rd_toastoid;
	else
		toast_pointer.va_toastrelid = RelationGetRelid(toastrel);

	/*
	 * Choose an unused OID within the toast table for this toast value.
	 */
	toast_pointer.va_valueid = GetNewOidWithIndex(toastrel,
												  RelationGetRelid(toastidx),
												  (AttrNumber) 1);

#ifdef USE_ASSERT_CHECKING
	Assert((VARATT_IS_COMPRESSED(value) || 0) == (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) || 0));

	if (VARATT_IS_COMPRESSED(value)) 
	{
		Assert(VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
		elog(DEBUG4,
			 "saved toast datum, original varsize %ud rawsize %ud new extsize %ud rawsize %uld\n", 
			 VARSIZE(value), VARRAWSIZE_4B_C(value) + VARHDRSZ,
			 toast_pointer.va_extsize, toast_pointer.va_rawsize);
	}
	else
	{
		Assert(!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
		elog(DEBUG4,
			 "saved toast datum, original varsize %ud new extsize %ud rawsize %ud\n", 
			 VARSIZE(value),
			 toast_pointer.va_extsize, toast_pointer.va_rawsize);
	}
#endif

	/*
	 * Initialize constant parts of the tuple data
	 */
	t_values[0] = ObjectIdGetDatum(toast_pointer.va_valueid);
	t_values[2] = PointerGetDatum(&chunk_data);
	t_isnull[0] = false;
	t_isnull[1] = false;
	t_isnull[2] = false;

	/*
	 * GPDB: for upgrade testing purposes, allow the maximum chunk size to be
	 * overridden via GUC. The result must still fit into TOAST_MAX_CHUNK_SIZE
	 * so that it doesn't overflow our chunk_data struct.
	 */
	max_chunk_size = (gp_test_toast_max_chunk_size_override ?
					  gp_test_toast_max_chunk_size_override :
					  TOAST_MAX_CHUNK_SIZE);
	Assert(max_chunk_size <= TOAST_MAX_CHUNK_SIZE);

	/*
	 * Split up the item into chunks
	 */
	while (data_todo > 0)
	{
		/*
		 * Calculate the size of this chunk
		 */
		chunk_size = Min(max_chunk_size, data_todo);

		/*
		 * Build a tuple and store it
		 */
		t_values[1] = Int32GetDatum(chunk_seq++);
		SET_VARSIZE(&chunk_data, chunk_size + VARHDRSZ);
		memcpy(VARDATA(&chunk_data), data_p, chunk_size);
		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);

		if (!isFrozen)
		{
			/* the normal case. regular insert */
			heap_insert(toastrel, toasttup, mycid, options, NULL, myxid);
		}
		else
		{
			/* insert and freeze the tuple. used for errtables and their related toast data */
			frozen_heap_insert(toastrel, toasttup);
		}
			
		/*
		 * Create the index entry.	We cheat a little here by not using
		 * FormIndexDatum: this relies on the knowledge that the index columns
		 * are the same as the initial columns of the table.
		 *
		 * Note also that there had better not be any user-created index on
		 * the TOAST table, since we don't bother to update anything else.
		 */
		index_insert(toastidx, t_values, t_isnull,
					 &(toasttup->t_self),
					 toastrel,
					 toastidx->rd_index->indisunique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO);

		/*
		 * Free memory
		 */
		heap_freetuple(toasttup);

		/*
		 * Move on to next chunk
		 */
		data_todo -= chunk_size;
		data_p += chunk_size;
	}

	/*
	 * Done - close toast relation
	 */
	index_close(toastidx, RowExclusiveLock);
	heap_close(toastrel, RowExclusiveLock);

	/*
	 * Create the TOAST pointer value that we'll return
	 */
	result = (struct varlena *) palloc(TOAST_POINTER_SIZE);
	SET_VARSIZE_EXTERNAL(result, TOAST_POINTER_SIZE);
	memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));

	return PointerGetDatum(result);
}


/* ----------
 * toast_delete_datum -
 *
 *	Delete a single external stored value.
 * ----------
 */
static void
toast_delete_datum(Relation rel __attribute__((unused)), Datum value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	Relation	toastrel;
	Relation	toastidx;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	toasttup;

	if (!VARATT_IS_EXTERNAL(attr))
		return;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * Open the toast relation and its index
	 */
	toastrel = heap_open(toast_pointer.va_toastrelid, RowExclusiveLock);
	toastidx = index_open(toastrel->rd_rel->reltoastidxid, RowExclusiveLock);

	check_toast_indisvalid(toastrel, toastidx);

	/*
	 * Setup a scan key to find chunks with matching va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Find all the chunks.  (We don't actually care whether we see them in
	 * sequence or not, but since we've already locked the index we might as
	 * well use systable_beginscan_ordered.)
	 */
	toastscan = systable_beginscan_ordered(toastrel, toastidx,
										   SnapshotToast, 1, &toastkey);
	while ((toasttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, delete it
		 */
		simple_heap_delete(toastrel, &toasttup->t_self);
	}

	/*
	 * End scan and close relations
	 */
	systable_endscan_ordered(toastscan);
	index_close(toastidx, RowExclusiveLock);
	heap_close(toastrel, RowExclusiveLock);
}


/* ----------
 * toast_fetch_datum -
 *
 *	Reconstruct an in memory Datum from the chunks saved
 *	in the toast relation
 * ----------
 */
static struct varlena *
toast_fetch_datum(struct varlena * attr)
{
	Relation	toastrel;
	Relation	toastidx;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	TupleDesc	toasttupDesc;
	struct varlena *result;
	struct varatt_external toast_pointer;
	int32		ressize;
	int32		residx,
				nextidx;
	int32		numchunks;
	Pointer		chunk;
	bool		isnull;
	char	   *chunkdata;
	int32		chunksize;

	/*
	 * GPDB: start with the assumption that chunks max out at
	 * TOAST_MAX_CHUNK_SIZE. This may later prove false (e.g. if we've upgraded
	 * from GPDB 4.3), in which case we'll readjust numchunks later.
	 */
	int32		actual_max_chunk_size = TOAST_MAX_CHUNK_SIZE;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	ressize = toast_pointer.va_extsize;
	numchunks = ((ressize - 1) / actual_max_chunk_size) + 1;

	result = (struct varlena *) palloc(ressize + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, ressize + VARHDRSZ);
	else
		SET_VARSIZE(result, ressize + VARHDRSZ);

	/*
	 * Open the toast relation and its index
	 */
	toastrel = heap_open(toast_pointer.va_toastrelid, AccessShareLock);
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(toastrel->rd_rel->reltoastidxid, AccessShareLock);

	check_toast_indisvalid(toastrel, toastidx);

	/*
	 * Setup a scan key to fetch from the index by va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Read the chunks by index
	 *
	 * Note that because the index is actually on (valueid, chunkidx) we will
	 * see the chunks in chunkidx order, even though we didn't explicitly ask
	 * for it.
	 */
	nextidx = 0;

	toastscan = systable_beginscan_ordered(toastrel, toastidx,
										   SnapshotToast, 1, &toastkey);
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* could happen due to heap_form_tuple doing its thing */
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* should never happen */
			elog(ERROR, "found toasted toast chunk for toast value %u in %s",
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
			chunksize = 0;		/* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if (residx != nextidx)
			elog(ERROR, "unexpected chunk number %d (expected %d) for toast value %u in %s",
				 residx, nextidx,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));

		if ((residx == 0) && (chunksize < ressize)
			&& (chunksize != actual_max_chunk_size))
		{
			/*
			 * GPDB: This toasted tuple is using a different max chunk size.
			 * This can happen after an upgrade, for instance. Realign our
			 * expectations.
			 *
			 * Only perform this check on the first chunk (the max size isn't
			 * allowed to change partway through), and only if we expect more
			 * chunks to come after this based on ressize.
			 */
			elog(DEBUG4, "readjusting max chunk size from %d to %d for toast value %u in %s",
				 actual_max_chunk_size, chunksize, toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));

			actual_max_chunk_size = chunksize;
			numchunks = ((ressize - 1) / actual_max_chunk_size) + 1;
		}

		if (residx < numchunks - 1)
		{
			if (chunksize != actual_max_chunk_size)
				elog(ERROR, "unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
					 chunksize, (int) actual_max_chunk_size,
					 residx, numchunks,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else if (residx == numchunks - 1)
		{
			if ((residx * actual_max_chunk_size + chunksize) != ressize)
				elog(ERROR, "unexpected chunk size %d (expected %d) in final chunk %d for toast value %u in %s",
					 chunksize,
					 (int) (ressize - residx * actual_max_chunk_size),
					 residx,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else
			elog(ERROR, "unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
				 residx,
				 0, numchunks - 1,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));

		/*
		 * Copy the data into proper place in our result
		 */
		memcpy(VARDATA(result) + residx * actual_max_chunk_size,
			   chunkdata,
			   chunksize);

		nextidx++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (nextidx != numchunks)
		elog(ERROR, "missing chunk number %d for toast value %u in %s",
			 nextidx,
			 toast_pointer.va_valueid,
			 RelationGetRelationName(toastrel));

	/*
	 * End scan and close relations
	 */
	systable_endscan_ordered(toastscan);
	index_close(toastidx, AccessShareLock);
	heap_close(toastrel, AccessShareLock);

	return (struct varlena *)result;
}

/* ----------
 * toast_fetch_datum_slice -
 *
 *	Reconstruct a segment of a Datum from the chunks saved
 *	in the toast relation
 * ----------
 */
static struct varlena *
toast_fetch_datum_slice(struct varlena * attr, int32 sliceoffset, int32 length)
{
	Relation	toastrel;
	Relation	toastidx;
	ScanKeyData toastkey[3];
	int			nscankeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	TupleDesc	toasttupDesc;
	struct varlena *result;
	struct varatt_external toast_pointer;
	int32		attrsize;
	int32		residx;
	int32		nextidx;
	int			numchunks;
	int			startchunk;
	int			endchunk;
	int32		startoffset;
	int32		endoffset;
	int			totalchunks;
	Pointer		chunk;
	bool		isnull;
	char	   *chunkdata;
	int32		chunksize;
	int32		chcpystrt;
	int32		chcpyend;

	/*
	 * GPDB: start with the assumption that chunks max out at
	 * TOAST_MAX_CHUNK_SIZE. This may later prove false (e.g. if we've upgraded
	 * from GPDB 4.3), in which case we'll readjust everything later.
	 */
	int32		actual_max_chunk_size = TOAST_MAX_CHUNK_SIZE;

	Assert(VARATT_IS_EXTERNAL(attr));

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * It's nonsense to fetch slices of a compressed datum -- this isn't lo_*
	 * we can't return a compressed datum which is meaningful to toast later
	 */
	Assert(!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));

	attrsize = toast_pointer.va_extsize;

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		length = 0;
	}

	if (((sliceoffset + length) > attrsize) || length < 0)
		length = attrsize - sliceoffset;

	result = (struct varlena *) palloc(length + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, length + VARHDRSZ);
	else
		SET_VARSIZE(result, length + VARHDRSZ);

	if (length == 0)
		return (struct varlena *)result;			/* Can save a lot of work at this point! */

	/*
	 * Open the toast relation and its index
	 */
	toastrel = heap_open(toast_pointer.va_toastrelid, AccessShareLock);
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(toastrel->rd_rel->reltoastidxid, AccessShareLock);

	check_toast_indisvalid(toastrel, toastidx);

	{
		/*
		 * GPDB: because we allow upgrades from clusters with different
		 * TOAST_MAX_CHUNK_SIZEs, we can't compute our chunk offsets yet. Open
		 * the first chunk and check its size.
		 */
		ScanKeyInit(&toastkey[0],
					(AttrNumber) 1,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(toast_pointer.va_valueid));
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(0));
		nscankeys = 2;

		toastscan = systable_beginscan_ordered(toastrel, toastidx,
											   SnapshotToast, nscankeys, toastkey);

		if ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
		{
			/*
			 * Have a chunk, extract the sequence number and the data
			 */
			residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
			Assert(!isnull);
			chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
			Assert(!isnull);

			if (!VARATT_IS_EXTENDED(chunk))
			{
				chunksize = VARSIZE(chunk) - VARHDRSZ;
			}
			else if (VARATT_IS_SHORT(chunk))
			{
				/* could happen due to heap_form_tuple doing its thing */
				chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			}
			else
			{
				/* should never happen */
				elog(ERROR, "found toasted toast chunk for toast value %u in %s",
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
				chunksize = 0;		/* keep compiler quiet */
			}

			if (chunksize < attrsize)
			{
				/*
				 * Only adjust the max chunk size if this isn't the only chunk.
				 */
				actual_max_chunk_size = chunksize;
			}
		}

		systable_endscan_ordered(toastscan);
	}

	totalchunks = ((attrsize - 1) / actual_max_chunk_size) + 1;

	startchunk = sliceoffset / actual_max_chunk_size;
	endchunk = (sliceoffset + length - 1) / actual_max_chunk_size;
	numchunks = (endchunk - startchunk) + 1;

	startoffset = sliceoffset % actual_max_chunk_size;
	endoffset = (sliceoffset + length - 1) % actual_max_chunk_size;

	/*
	 * Setup a scan key to fetch from the index. This is either two keys or
	 * three depending on the number of chunks.
	 */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Use equality condition for one chunk, a range condition otherwise:
	 */
	if (numchunks == 1)
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(startchunk));
		ScanKeyInit(&toastkey[2],
					(AttrNumber) 2,
					BTLessEqualStrategyNumber, F_INT4LE,
					Int32GetDatum(endchunk));
		nscankeys = 3;
	}

	/*
	 * Read the chunks by index
	 *
	 * The index is on (valueid, chunkidx) so they will come in order
	 */
	nextidx = startchunk;
	toastscan = systable_beginscan_ordered(toastrel, toastidx,
										 SnapshotToast, nscankeys, toastkey);
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* could happen due to heap_form_tuple doing its thing */
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* should never happen */
			elog(ERROR, "found toasted toast chunk for toast value %u in %s",
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
			chunksize = 0;		/* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if ((residx != nextidx) || (residx > endchunk) || (residx < startchunk))
			elog(ERROR, "unexpected chunk number %d (expected %d) for toast value %u in %s",
				 residx, nextidx,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
		if (residx < totalchunks - 1)
		{
			if (chunksize != actual_max_chunk_size)
				elog(ERROR, "unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s when fetching slice",
					 chunksize, (int) actual_max_chunk_size,
					 residx, totalchunks,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else if (residx == totalchunks - 1)
		{
			if ((residx * actual_max_chunk_size + chunksize) != attrsize)
				elog(ERROR, "unexpected chunk size %d (expected %d) in final chunk %d for toast value %u in %s when fetching slice",
					 chunksize,
					 (int) (attrsize - residx * actual_max_chunk_size),
					 residx,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else
			elog(ERROR, "unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
				 residx,
				 0, totalchunks - 1,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));

		/*
		 * Copy the data into proper place in our result
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (residx == startchunk)
			chcpystrt = startoffset;
		if (residx == endchunk)
			chcpyend = endoffset;

		memcpy(VARDATA(result) +
			   (residx * actual_max_chunk_size - sliceoffset) + chcpystrt,
			   chunkdata + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		nextidx++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (nextidx != (endchunk + 1))
		elog(ERROR, "missing chunk number %d for toast value %u in %s",
			 nextidx,
			 toast_pointer.va_valueid,
			 RelationGetRelationName(toastrel));

	/*
	 * End scan and close relations
	 */
	systable_endscan_ordered(toastscan);
	index_close(toastidx, AccessShareLock);
	heap_close(toastrel, AccessShareLock);

	return (struct varlena *)result;
}
