/*
** Copyright (C) 2001-2014 by Carnegie Mellon University.
**
** @OPENSOURCE_HEADER_START@
**
** Use of the SILK system and related source code is subject to the terms
** of the following licenses:
**
** GNU Public License (GPL) Rights pursuant to Version 2, June 1991
** Government Purpose License Rights (GPLR) pursuant to DFARS 252.227.7013
**
** NO WARRANTY
**
** ANY INFORMATION, MATERIALS, SERVICES, INTELLECTUAL PROPERTY OR OTHER
** PROPERTY OR RIGHTS GRANTED OR PROVIDED BY CARNEGIE MELLON UNIVERSITY
** PURSUANT TO THIS LICENSE (HEREINAFTER THE "DELIVERABLES") ARE ON AN
** "AS-IS" BASIS. CARNEGIE MELLON UNIVERSITY MAKES NO WARRANTIES OF ANY
** KIND, EITHER EXPRESS OR IMPLIED AS TO ANY MATTER INCLUDING, BUT NOT
** LIMITED TO, WARRANTY OF FITNESS FOR A PARTICULAR PURPOSE,
** MERCHANTABILITY, INFORMATIONAL CONTENT, NONINFRINGEMENT, OR ERROR-FREE
** OPERATION. CARNEGIE MELLON UNIVERSITY SHALL NOT BE LIABLE FOR INDIRECT,
** SPECIAL OR CONSEQUENTIAL DAMAGES, SUCH AS LOSS OF PROFITS OR INABILITY
** TO USE SAID INTELLECTUAL PROPERTY, UNDER THIS LICENSE, REGARDLESS OF
** WHETHER SUCH PARTY WAS AWARE OF THE POSSIBILITY OF SUCH DAMAGES.
** LICENSEE AGREES THAT IT WILL NOT MAKE ANY WARRANTY ON BEHALF OF
** CARNEGIE MELLON UNIVERSITY, EXPRESS OR IMPLIED, TO ANY PERSON
** CONCERNING THE APPLICATION OF OR THE RESULTS TO BE OBTAINED WITH THE
** DELIVERABLES UNDER THIS LICENSE.
**
** Licensee hereby agrees to defend, indemnify, and hold harmless Carnegie
** Mellon University, its trustees, officers, employees, and agents from
** all claims or demands made against them (and any related losses,
** expenses, or attorney's fees) arising out of, or relating to Licensee's
** and/or its sub licensees' negligent use or willful misuse of or
** negligent conduct or willful misconduct regarding the Software,
** facilities, or other rights or assistance granted by Carnegie Mellon
** University under this License, including, but not limited to, any
** claims of product liability, personal injury, death, damage to
** property, or violation of any laws or regulations.
**
** Carnegie Mellon University Software Engineering Institute authored
** documents are sponsored by the U.S. Department of Defense under
** Contract FA8721-05-C-0003. Carnegie Mellon University retains
** copyrights in all material produced under this contract. The U.S.
** Government retains a non-exclusive, royalty-free license to publish or
** reproduce these documents, or allow others to do so, for U.S.
** Government purposes only pursuant to the copyright license under the
** contract clause at 252.227.7013.
**
** @OPENSOURCE_HEADER_END@
*/

/*
**  rwsort reads SiLK Flow Records from the standard input or from
**  named files and sorts them on one or more user-specified fields.
**
**  rwsort attempts to sort the records in RAM using a buffer whose
**  maximum size is DEFAULT_SORT_BUFFER_SIZE bytes.  The user may
**  choose a different maximum size with the --sort-buffer-size
**  switch.  The buffer rwsort initially allocates is
**  1/SORT_NUM_CHUNKS of this size; when it is full, the buffer is
**  reallocated and grown by another 1/SORT_NUM_CHUNKS.  This
**  continues until all records are read, a realloc() fails, or the
**  maximum buffer size is reached.
**
**  The purpose of gradually increasing the buffer size is twofold:
**  1. So we don't use more memory than we actually need.  2. When
**  allocating a large buffer during start-up, the OS would give us
**  the memory, but if we attempted to use the buffer the OS would
**  kill the rwsort process.
**
**  Records are read and stored in this buffer; if the input ends
**  before the buffer is filled, the records are sorted and printed to
**  standard out or to the named output file.
**
**  However, if the buffer fills before the input is completely read,
**  the records in the buffer are sorted and written to a temporary
**  file on disk; the buffer is cleared, and reading of the input
**  resumes, repeating the process as necessary until all records are
**  read.  We then do an N-way merge-sort on the temporary files,
**  where N is either all the temporary files, MAX_MERGE_FILES, or the
**  maximum number that we can open before running out of file descriptors
**  (EMFILE) or memory.  If we cannot open all temporary files, we
**  merge the N files into a new temporary file, then add it to the
**  list of files to merge.
**
**  When the temporary files are written to the same volume (file
**  system) as the final output, the maximum disk usage will be
**  2-times the number of records read (times the size per record);
**  when different volumes are used, the disk space required for the
**  temporary files will be between 1 and 1.5 times the number of
**  records.
**
*/

#include <silk/silk.h>

RCSIDENT("$SiLK: rwsort.c cd598eff62b9 2014-09-21 19:31:29Z mthomas $");

#include "rwsort.h"
#include <silk/skheap.h>


/* EXPORTED VARIABLES */

/* number of fields to sort over; skStringMapParse() sets this */
uint32_t num_fields = 0;

/* IDs of the fields to sort over; skStringMapParse() sets it; values
 * are from the rwrec_printable_fields_t enum and from values that
 * come from plug-ins. */
uint32_t *sort_fields = NULL;

/* the size of a "node".  Because the output from rwsort are SiLK
 * records, the node size includes the complete rwRec, plus any binary
 * fields that we get from plug-ins to use as the key.  This node_size
 * value may increase when we parse the --fields switch. */
uint32_t node_size = sizeof(rwRec);

/* the columns that make up the key that come from plug-ins */
key_field_t key_fields[MAX_PLUGIN_KEY_FIELDS];

/* the number of these key_fields */
size_t key_num_fields = 0;

/* output stream */
skstream_t *out_rwios = NULL;

/* temp file context */
sk_tempfilectx_t *tmpctx;

/* whether the user wants to reverse the sort order */
int reverse = 0;

/* whether to treat the input files as already sorted */
int presorted_input = 0;

/* maximum amount of RAM to attempt to allocate */
uint64_t sort_buffer_size = DEFAULT_SORT_BUFFER_SIZE;



/* FUNCTION DEFINITIONS */


/* How to sort the flows: forward or reverse? */
#define RETURN_SORT_ORDER(val)                  \
    return (reverse ? -(val) : (val))

/* Define our raw sorting functions */
#define RETURN_IF_SORTED(func, rec_a, rec_b)                    \
    {                                                           \
        if (func((rwRec*)(rec_a)) < func((rwRec*)(rec_b))) {    \
            RETURN_SORT_ORDER(-1);                              \
        }                                                       \
        if (func((rwRec*)(rec_a)) > func((rwRec*)(rec_b))) {    \
            RETURN_SORT_ORDER(1);                               \
        }                                                       \
    }

#define RETURN_IF_SORTED_IPS(func, rec_a, rec_b)        \
    {                                                   \
        skipaddr_t ipa, ipb;                            \
        int cmp;                                        \
        func((rwRec*)(rec_a), &ipa);                    \
        func((rwRec*)(rec_b), &ipb);                    \
        cmp = skipaddrCompare(&ipa, &ipb);              \
        if (cmp != 0) {                                 \
            RETURN_SORT_ORDER(cmp);                     \
        }                                               \
    }


static uint8_t
getIcmpType(
    const void         *rec)
{
    if (rwRecIsICMP((rwRec*)rec)) {
        return rwRecGetIcmpType((rwRec*)rec);
    }
    return 0;
}

static uint8_t
getIcmpCode(
    const void         *rec)
{
    if (rwRecIsICMP((rwRec*)rec)) {
        return rwRecGetIcmpCode((rwRec*)rec);
    }
    return 0;
}


/*
 *  rwrecCompare(a, b);
 *
 *     Returns an ordering on the recs pointed to `a' and `b' by
 *     comparing the fields listed in the sort_fields[] array.
 */
static int
rwrecCompare(
    const void         *a,
    const void         *b)
{
    key_field_t *key = key_fields;
    skplugin_err_t err;
    uint32_t i;
    int rv;

    for (i = 0; i < num_fields; ++i) {
        switch (sort_fields[i]) {
          case RWREC_FIELD_SIP:
#if !SK_ENABLE_IPV6
            RETURN_IF_SORTED(rwRecGetSIPv4, a, b);
#else
            RETURN_IF_SORTED_IPS(rwRecMemGetSIP, a, b);
#endif /* SK_ENABLE_IPV6 */
            break;

          case RWREC_FIELD_DIP:
#if !SK_ENABLE_IPV6
            RETURN_IF_SORTED(rwRecGetDIPv4, a, b);
#else
            RETURN_IF_SORTED_IPS(rwRecMemGetDIP, a, b);
#endif /* SK_ENABLE_IPV6 */
            break;

          case RWREC_FIELD_NHIP:
#if !SK_ENABLE_IPV6
            RETURN_IF_SORTED(rwRecGetNhIPv4, a, b);
#else
            RETURN_IF_SORTED_IPS(rwRecMemGetNhIP, a, b);
#endif /* SK_ENABLE_IPV6 */
            break;

          case RWREC_FIELD_SPORT:
            RETURN_IF_SORTED(rwRecGetSPort, a, b);
            break;

          case RWREC_FIELD_DPORT:
            RETURN_IF_SORTED(rwRecGetDPort, a, b);
            break;

          case RWREC_FIELD_PROTO:
            RETURN_IF_SORTED(rwRecGetProto, a, b);
            break;

          case RWREC_FIELD_PKTS:
            RETURN_IF_SORTED(rwRecGetPkts, a, b);
            break;

          case RWREC_FIELD_BYTES:
            RETURN_IF_SORTED(rwRecGetBytes, a, b);
            break;

          case RWREC_FIELD_FLAGS:
            RETURN_IF_SORTED(rwRecGetFlags, a, b);
            break;

          case RWREC_FIELD_STIME:
          case RWREC_FIELD_STIME_MSEC:
            RETURN_IF_SORTED(rwRecGetStartTime, a, b);
            break;

          case RWREC_FIELD_ELAPSED:
          case RWREC_FIELD_ELAPSED_MSEC:
            RETURN_IF_SORTED(rwRecGetElapsed, a, b);
            break;

          case RWREC_FIELD_ETIME:
          case RWREC_FIELD_ETIME_MSEC:
            RETURN_IF_SORTED(rwRecGetEndTime, a, b);
            break;

          case RWREC_FIELD_SID:
            RETURN_IF_SORTED(rwRecGetSensor, a, b);
            break;

          case RWREC_FIELD_INPUT:
            RETURN_IF_SORTED(rwRecGetInput, a, b);
            break;

          case RWREC_FIELD_OUTPUT:
            RETURN_IF_SORTED(rwRecGetOutput, a, b);
            break;

          case RWREC_FIELD_INIT_FLAGS:
            RETURN_IF_SORTED(rwRecGetInitFlags, a, b);
            break;

          case RWREC_FIELD_REST_FLAGS:
            RETURN_IF_SORTED(rwRecGetRestFlags, a, b);
            break;

          case RWREC_FIELD_TCP_STATE:
            RETURN_IF_SORTED(rwRecGetTcpState, a, b);
            break;

          case RWREC_FIELD_APPLICATION:
            RETURN_IF_SORTED(rwRecGetApplication, a, b);
            break;

          case RWREC_FIELD_FTYPE_CLASS:
          case RWREC_FIELD_FTYPE_TYPE:
            RETURN_IF_SORTED(rwRecGetFlowType, a, b);
            break;

          case RWREC_FIELD_ICMP_TYPE:
            RETURN_IF_SORTED(getIcmpType, a, b);
            break;

          case RWREC_FIELD_ICMP_CODE:
            RETURN_IF_SORTED(getIcmpCode, a, b);
            break;

#if 0
          case RWREC_FIELD_SCC:
            {
                uint16_t a_cc = skCountryLookupCode(rwRecGetSIPv4((rwRec*)a));
                uint16_t b_cc = skCountryLookupCode(rwRecGetSIPv4((rwRec*)b));
                if (a_cc < b_cc) {
                    RETURN_SORT_ORDER(-1);
                } else if (b_cc > a_cc) {
                    RETURN_SORT_ORDER(1);
                }
            }
            break;

          case RWREC_FIELD_DCC:
            {
                uint16_t a_cc = skCountryLookupCode(rwRecGetDIPv4((rwRec*)a));
                uint16_t b_cc = skCountryLookupCode(rwRecGetDIPv4((rwRec*)b));
                if (a_cc < b_cc) {
                    RETURN_SORT_ORDER(-1);
                } else if (b_cc > a_cc) {
                    RETURN_SORT_ORDER(1);
                }
            }
            break;
#endif  /* 0 */

          default:
            /* we go through the fields in the same way they were
             * added, and 'key' should always be an index to the
             * current plugin. */
            assert((size_t)(key - key_fields) < key_num_fields);
            err=skPluginFieldRunBinCompareFn(key->kf_field_handle, &rv,
                                             &(((uint8_t*)a)[key->kf_offset]),
                                             &(((uint8_t*)b)[key->kf_offset]));
            if (err != SKPLUGIN_OK) {
                const char **name;
                skPluginFieldName(key->kf_field_handle, &name);
                skAppPrintErr(("Plugin-based field %s failed "
                               "comparing binary values "
                               "with error code %d"), name[0], err);
                exit(EXIT_FAILURE);
            }
            ++key;
            if (rv != 0) {
                RETURN_SORT_ORDER(rv);
            }
            break;
        }
    }

    return 0;
}


/*
 *  status = compHeapNodes(b, a, v_recs);
 *
 *    Callback function used by the heap two compare two heapnodes,
 *    there are just indexes into an array of records.  'v_recs' is
 *    the array of records, where each record is MAX_NODE_SIZE bytes.
 *
 *    Note the order of arguments is 'b', 'a'.
 */
static int
compHeapNodes(
    const skheapnode_t  b,
    const skheapnode_t  a,
    void               *v_recs)
{
    uint8_t *recs = (uint8_t*)v_recs;

    return rwrecCompare(&recs[*(uint16_t*)a * MAX_NODE_SIZE],
                        &recs[*(uint16_t*)b * MAX_NODE_SIZE]);
}


/*
 *  status = fillRecordAndKey(rwios, buf);
 *
 *    Reads a flow record from 'rwios', computes the key based on the
 *    global key_fields[] settings, and fills in the parameter 'buf'
 *    with the record and then the key.  Return 1 if a record was
 *    read, or 0 if it was not.
 */
static int
fillRecordAndKey(
    skstream_t         *rwios,
    uint8_t            *buf)
{
    rwRec *rwrec = (rwRec*)buf;
    skplugin_err_t err;
    const char **name;
    size_t i;
    int rv;

    rv = skStreamReadRecord(rwios, rwrec);
    if (rv) {
        /* end of file or error getting record */
        if (SKSTREAM_ERR_EOF != rv) {
            skStreamPrintLastErr(rwios, rv, &skAppPrintErr);
        }
        return 0;
    }

    /* lookup data from plug-in */
    for (i = 0; i < key_num_fields; ++i) {
        err = skPluginFieldRunRecToBinFn(key_fields[i].kf_field_handle,
                                         &(buf[key_fields[i].kf_offset]),
                                         rwrec, NULL);
        if (err != SKPLUGIN_OK) {
            skPluginFieldName(key_fields[i].kf_field_handle, &name);
            skAppPrintErr(("Plugin-based field %s failed "
                           "converting to binary "
                           "with error code %d"), name[0], err);
            appExit(EXIT_FAILURE);
        }
    }
    return 1;
}


/*
 *  mergeFiles(temp_file_idx)
 *
 *    Merge the temporary files numbered from 0 to 'temp_file_idx'
 *    inclusive into the output file 'out_ios', maintaining sorted
 *    order.  Exits the application if an error occurs.
 */
static void
mergeFiles(
    int                 temp_file_idx)
{
    FILE *fps[MAX_MERGE_FILES];
    uint8_t recs[MAX_MERGE_FILES][MAX_NODE_SIZE];
    int j;
    uint16_t open_count;
    uint16_t i;
    uint16_t *top_heap;
    uint16_t lowest;
    int tmp_idx_a;
    int tmp_idx_b;
    FILE *fp_intermediate = NULL;
    int tmp_idx_intermediate;
    int no_more_temps = 0;
    skheap_t *heap;
    int rv;

    TRACEMSG(("Merging #%d through #%d into '%s'",
              0, temp_file_idx, skStreamGetPathname(out_rwios)));

    heap = skHeapCreate2(compHeapNodes, MAX_MERGE_FILES, sizeof(uint16_t),
                         NULL, recs);
    if (NULL == heap) {
        skAppPrintOutOfMemory("heap");
        appExit(EXIT_FAILURE);
    }

    /* the index of the first temp file to the merge */
    tmp_idx_a = 0;

    /* This loop repeats as long as we haven't read all of the temp
     * files generated in the sorting stage. */
    do {
        assert(SKHEAP_ERR_EMPTY==skHeapPeekTop(heap,(skheapnode_t*)&top_heap));

        /* the index of the last temp file to merge */
        if (temp_file_idx - tmp_idx_a < MAX_MERGE_FILES - 1) {
            tmp_idx_b = temp_file_idx;
        } else {
            tmp_idx_b = tmp_idx_a + MAX_MERGE_FILES - 1;
        }

        /* open an intermediate temp file.  The merge-sort will have
         * to write records here if there are not enough file handles
         * available to open all the existing tempoary files. */
        fp_intermediate = skTempFileCreate(tmpctx, &tmp_idx_intermediate,NULL);
        if (fp_intermediate == NULL) {
            skAppPrintSyserror("Error creating new temporary file");
            appExit(EXIT_FAILURE);
        }

        /* count number of files we open */
        open_count = 0;

        /* Attempt to open up to MAX_MERGE_FILES, though we an open
         * may fail due to lack of resources (EMFILE or ENOMEM) */
        for (j = tmp_idx_a; j <= tmp_idx_b; ++j) {
            fps[open_count] = skTempFileOpen(tmpctx, j);
            if (fps[open_count] == NULL) {
                if ((open_count > 0)
                    && ((errno == EMFILE) || (errno == ENOMEM)))
                {
                    /* Blast!  We can't open any more temp files.  So,
                     * we rewind by one to catch this one the next
                     * time around. */
                    tmp_idx_b = j - 1;
                    TRACEMSG((("EMFILE limit hit--"
                               "merging #%d through #%d into #%d"),
                              tmp_idx_a, tmp_idx_b, tmp_idx_intermediate));
                    break;
                } else {
                    skAppPrintSyserror(("Error opening existing"
                                        " temporary file '%s'"),
                                       skTempFileGetName(tmpctx, j));
                    appExit(EXIT_FAILURE);
                }
            }

            /* read the first record */
            if (!fread(recs[open_count], node_size, 1, fps[open_count])) {
                if (feof(fps[open_count])) {
                    TRACEMSG(("Ignoring empty temporary file '%s'",
                              skTempFileGetName(tmpctx, j)));
                    continue;
                }
                skAppPrintSyserror(("Error reading first record from"
                                    " temporary file '%s'"),
                                   skTempFileGetName(tmpctx, j));
                appExit(EXIT_FAILURE);
            }

            /* insert the file index into the heap */
            skHeapInsert(heap, &open_count);
            ++open_count;
        }

        /* Here, we check to see if we've opened all temp files.  If
         * so, set a flag so we write data to final destination and
         * break out of the loop after we're done. */
        if (tmp_idx_b == temp_file_idx) {
            no_more_temps = 1;
            /* no longer need the intermediate temp file */
            fclose(fp_intermediate);
            fp_intermediate = NULL;
        } else {
            /* we could not open all temp files, so merge all opened
             * temp files into the intermediate file.  Add the
             * intermediate file to the list of files to merge */
            temp_file_idx = tmp_idx_intermediate;
        }

        TRACEMSG((("Merging %" PRIu16 " temporary files"), open_count));

        /* exit this while() once we are only processing a single
         * file */
        while (skHeapGetNumberEntries(heap) > 1) {
            /* entry at the top of the heap has the lowest key */
            skHeapPeekTop(heap, (skheapnode_t*)&top_heap);
            lowest = *top_heap;

            /* write the lowest record */
            if (fp_intermediate) {
                /* write record to intermediate tmp file */
                if (!fwrite(recs[lowest], node_size, 1, fp_intermediate)) {
                    skAppPrintSyserror(("Error writing record to"
                                        " temporary file '%s'"),
                                       skTempFileGetName(tmpctx,
                                                         tmp_idx_intermediate));
                    appExit(EXIT_FAILURE);
                }
            } else {
                /* we successfully opened all (remaining) temp files,
                 * write to record to the final destination */
                rv = skStreamWriteRecord(out_rwios,(rwRec*)recs[lowest]);
                if (0 != rv) {
                    skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
                    if (SKSTREAM_ERROR_IS_FATAL(rv)) {
                        appExit(EXIT_FAILURE);
                    }
                }
            }

            /* replace the record we just wrote */
            if (fread(recs[lowest], node_size, 1, fps[lowest])) {
                /* read was successful.  "insert" the new entry into
                 * the heap (which has same value as old entry). */
                skHeapReplaceTop(heap, &lowest, NULL);
            } else {
                TRACEMSG(("Finished reading records from file #%u",
                          lowest));
                /* no more data for this file; remove it from the
                 * heap */
                skHeapExtractTop(heap, NULL);
            }
        }

        /* get index of the remaining file */
        skHeapExtractTop(heap, &lowest);
        assert(SKHEAP_ERR_EMPTY==skHeapPeekTop(heap,(skheapnode_t*)&top_heap));

        /* read records from the remaining file */
        if (fp_intermediate) {
            do {
                if (!fwrite(recs[lowest], node_size, 1, fp_intermediate)) {
                    skAppPrintErr("Error writing record to temporary file '%s'",
                                  skTempFileGetName(tmpctx,
                                                    tmp_idx_intermediate));
                    appExit(EXIT_FAILURE);
                }
            } while (fread(recs[lowest], node_size, 1, fps[lowest]));
        } else {
            do {
                rv = skStreamWriteRecord(out_rwios, (rwRec*)recs[lowest]);
                if (0 != rv) {
                    skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
                    if (SKSTREAM_ERROR_IS_FATAL(rv)) {
                        appExit(EXIT_FAILURE);
                    }
                }
            } while (fread(recs[lowest], node_size, 1, fps[lowest]));
        }

        TRACEMSG(("Finished reading records from file #%u", lowest));
        TRACEMSG((("Finished processing #%d through #%d"),
                  tmp_idx_a, tmp_idx_b));

        /* Close all open temp files */
        for (i = 0; i < open_count; ++i) {
            fclose(fps[i]);
        }
        /* Delete all temp files we opened (or attempted to open) this
         * time */
        for (j = tmp_idx_a; j <= tmp_idx_b; ++j) {
            skTempFileRemove(tmpctx, j);
        }

        /* Close the intermediate temp file. */
        if (fp_intermediate) {
            if (EOF == fclose(fp_intermediate)) {
                skAppPrintSyserror("Error closing temporary file '%s'",
                                   skTempFileGetName(tmpctx,
                                                     tmp_idx_intermediate));
                appExit(EXIT_FAILURE);
            }
            fp_intermediate = NULL;
        }

        /* Start the next merge with the next input temp file */
        tmp_idx_a = tmp_idx_b + 1;

    } while (!no_more_temps);

    skHeapFree(heap);
}


/*
 *  temp_file_idx = sortPresorted();
 *
 *    Assume all input files have been sorted using the exact same
 *    --fields value as those we are using, and simply merge sort
 *    them.
 *
 *    This function is still fairly complicated, because we have to
 *    handle running out of memory or file descriptors as we process
 *    the inputs.  When that happens, we write the records to
 *    temporary files and then use mergeFiles() above to sort those
 *    files.
 *
 *    Exits the application if an error occurs.  On success, this
 *    function returns the index of the final temporary file to use
 *    for the mergeSort().  A return value less than 0 is considered
 *    successful and indicates that no merge-sort is required.
 */
static int
sortPresorted(
    void)
{
    skstream_t *rwios[MAX_MERGE_FILES];
    uint8_t recs[MAX_MERGE_FILES][MAX_NODE_SIZE];
    uint16_t i;
    uint16_t open_count;
    uint16_t *top_heap;
    uint16_t lowest;
    FILE *fp_intermediate = NULL;
    int temp_file_idx = -1;
    int no_more_inputs = 0;
    skheap_t *heap;
    int rv;

    memset(rwios, 0, sizeof(rwios));
    memset(recs, 0, sizeof(recs));

    heap = skHeapCreate2(compHeapNodes, MAX_MERGE_FILES, sizeof(uint16_t),
                         NULL, recs);
    if (NULL == heap) {
        skAppPrintOutOfMemory("heap");
        appExit(EXIT_FAILURE);
    }

    /* This loop repeats as long as we haven't read all of input
     * files */
    do {
        /* open an intermediate temp file.  The merge-sort will have
         * to write records here if there are not enough file handles
         * available to open all the input files. */
        fp_intermediate = skTempFileCreate(tmpctx, &temp_file_idx, NULL);
        if (fp_intermediate == NULL) {
            skAppPrintSyserror("Error creating new temporary file");
            appExit(EXIT_FAILURE);
        }

        /* Attempt to open up to MAX_MERGE_FILES, though we an open
         * may fail due to lack of resources (EMFILE or ENOMEM) */
        for (open_count = 0; open_count < MAX_MERGE_FILES; ++open_count) {
            rv = appNextInput(&(rwios[open_count]));
            if (rv != 0) {
                break;
            }
        }
        switch (rv) {
          case 1:
            /* successfully opened all (remaining) input files */
            TRACEMSG(("Opened all (remaining) inputs"));
            no_more_inputs = 1;
            if (temp_file_idx == 0) {
                /* we opened all the input files in a single pass.  we
                 * no longer need the intermediate temp file */
                fclose(fp_intermediate);
                fp_intermediate = NULL;
                temp_file_idx = -1;
            }
            break;
          case -1:
            /* unexpected error opening a file */
            appExit(EXIT_FAILURE);
          case -2:
            /* ran out of memory or file descriptors */
            TRACEMSG((("Unable to open all inputs---"
                       "out of memory or file handles")));
            break;
          case 0:
            if (open_count == MAX_MERGE_FILES) {
                /* ran out of pointers for this run */
                TRACEMSG((("Unable to open all inputs---"
                           "MAX_MERGE_FILES limit reached")));
                break;
            }
            /* no other way that rv == 0 */
            TRACEMSG(("rv == 0 but open_count is %d. Abort.",
                      open_count));
            skAbort();
          default:
            /* unexpected error */
            TRACEMSG(("Got unexpected rv value = %d", rv));
            skAbortBadCase(rv);
        }

        /* Read the first record from each file into the work buffer */
        for (i = 0; i < open_count; ++i) {
            if (fillRecordAndKey(rwios[i], recs[i])) {
                /* insert the file index into the heap */
                skHeapInsert(heap, &i);
            }
        }

        TRACEMSG((("Merging %" PRIu32 " presorted files"),
                  skHeapGetNumberEntries(heap)));

        /* exit this while() once we are only processing a single
         * file */
        while (skHeapGetNumberEntries(heap) > 1) {
            /* entry at the top of the heap has the lowest key */
            skHeapPeekTop(heap, (skheapnode_t*)&top_heap);
            lowest = *top_heap;

            /* write the lowest record */
            if (fp_intermediate) {
                /* we are using the intermediate temp file, so
                 * write the record there. */
                if (!fwrite(recs[lowest], node_size, 1, fp_intermediate)) {
                    skAppPrintSyserror(("Error writing record to"
                                        " temporary file '%s'"),
                                       skTempFileGetName(tmpctx,
                                                         temp_file_idx));
                    appExit(EXIT_FAILURE);
                }
            } else {
                /* we are not using any temp files, write the
                 * record to the final destination */
                rv = skStreamWriteRecord(out_rwios, (rwRec*)recs[lowest]);
                if (0 != rv) {
                    skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
                    if (SKSTREAM_ERROR_IS_FATAL(rv)) {
                        appExit(EXIT_FAILURE);
                    }
                }
            }

            /* replace the record we just wrote */
            if (fillRecordAndKey(rwios[lowest], recs[lowest])) {
                /* read was successful.  "insert" the new entry into
                 * the heap (which has same value as old entry). */
                skHeapReplaceTop(heap, &lowest, NULL);
            } else {
                TRACEMSG(("Finished reading records from file #%u",
                          lowest));
                /* no more data for this file; remove it from the
                 * heap */
                skHeapExtractTop(heap, NULL);
            }
        }

        /* read records from the remaining file */
        if (SKHEAP_OK == skHeapExtractTop(heap, &lowest)) {
            if (fp_intermediate) {
                do {
                    if (!fwrite(recs[lowest], node_size, 1,fp_intermediate)) {
                        skAppPrintSyserror(("Error writing record to"
                                            " temporary file '%s'"),
                                           skTempFileGetName(tmpctx,
                                                             temp_file_idx));
                        appExit(EXIT_FAILURE);
                    }
                } while (fillRecordAndKey(rwios[lowest], recs[lowest]));
            } else {
                do {
                    rv = skStreamWriteRecord(out_rwios, (rwRec*)recs[lowest]);
                    if (0 != rv) {
                        skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
                        if (SKSTREAM_ERROR_IS_FATAL(rv)) {
                            appExit(EXIT_FAILURE);
                        }
                    }
                } while (fillRecordAndKey(rwios[lowest], recs[lowest]));
            }
            TRACEMSG(("Finished reading records from file #%u",
                      lowest));
        }

        /* Close the input files that we processed this time. */
        for (i = 0; i < open_count; ++i) {
            skStreamDestroy(&rwios[i]);
        }

        /* Close the intermediate temp file. */
        if (fp_intermediate) {
            if (EOF == fclose(fp_intermediate)) {
                skAppPrintSyserror("Error closing temporary file '%s'",
                                   skTempFileGetName(tmpctx, temp_file_idx));
                appExit(EXIT_FAILURE);
            }
            fp_intermediate = NULL;
        }
    } while (!no_more_inputs);

    skHeapFree(heap);

    /* If any temporary files were written, we now have to merge-sort
     * them */
    return temp_file_idx;
}


/*
 *  int = sortRandom();
 *
 *    Don't make any assumptions about the input.  Store the input
 *    records in a large buffer, and sort those in-core records once
 *    all records are processed or the buffer is full.  If the buffer
 *    fills up, store the sorted records into temporary files.  Once
 *    all records are read, use mergeFiles() above to merge-sort the
 *    temporary files.
 *
 *    Exits the application if an error occurs.  On success, this
 *    function returns the index of the final temporary file to use
 *    for the mergeSort().  A return value less than 0 is considered
 *    successful and indicates that no merge-sort is required.
 */
static int
sortRandom(
    void)
{
    int temp_file_idx = -1;
    skstream_t *input_rwios;        /* input stream */
    uint8_t *record_buffer = NULL;  /* Region of memory for records */
    uint8_t *cur_node = NULL;       /* Ptr into record_buffer */
    uint32_t buffer_max_recs;       /* max buffer size (in number of recs) */
    uint32_t buffer_recs;           /* current buffer size (# records) */
    uint32_t buffer_chunk_recs;     /* how to grow from current to max buf */
    uint32_t num_chunks;            /* how quickly to grow buffer */
    uint32_t record_count = 0;      /* Number of records read */
    int rv;

    /* Determine the maximum number of records that will fit into the
     * buffer if it grows the maximum size */
    buffer_max_recs = sort_buffer_size / node_size;
    TRACEMSG((("sort_buffer_size = %" PRIu64
               "\nnode_size = %" PRIu32
               "\nbuffer_max_recs = %" PRIu32),
              sort_buffer_size, node_size, buffer_max_recs));

    /* We will grow to the maximum size in chunks */
    num_chunks = SORT_NUM_CHUNKS;
    if (num_chunks <= 0) {
        num_chunks = 1;
    }

    /* Attempt to allocate the initial chunk.  If we fail, increment
     * the number of chunks---which will decrease the amount we
     * attempt to allocate at once---and try again. */
    for (;;) {
        buffer_chunk_recs = buffer_max_recs / num_chunks;
        TRACEMSG((("num_chunks = %" PRIu32
                   "\nbuffer_chunk_recs = %" PRIu32),
                  num_chunks, buffer_chunk_recs));

        record_buffer = (uint8_t*)calloc(buffer_chunk_recs, node_size);
        if (record_buffer) {
            /* malloc was successful */
            break;
        } else if (buffer_chunk_recs < MIN_IN_CORE_RECORDS) {
            /* give up at this point */
            skAppPrintErr("Error allocating space for %d records",
                          MIN_IN_CORE_RECORDS);
            appExit(EXIT_FAILURE);
        } else {
            /* reduce the amount we allocate at once by increasing the
             * number of chunks and try again */
            TRACEMSG(("malloc() failed"));
            ++num_chunks;
        }
    }

    buffer_recs = buffer_chunk_recs;
    TRACEMSG((("buffer_recs = %" PRIu32), buffer_recs));

    /* open first file */
    rv = appNextInput(&input_rwios);
    if (rv) {
        free(record_buffer);
        if (1 == rv) {
            return temp_file_idx;
        }
        appExit(EXIT_FAILURE);
    }

    record_count = 0;
    cur_node = record_buffer;
    while (input_rwios != NULL) {
        /* read record */
        rv = fillRecordAndKey(input_rwios, cur_node);
        if (rv == 0) {
            /* close current and open next */
            skStreamDestroy(&input_rwios);
            rv = appNextInput(&input_rwios);
            if (rv < 0) {
                /* processing these input files one at a time, so we
                 * will not hit the EMFILE limit here */
                free(record_buffer);
                appExit(EXIT_FAILURE);
            }
            continue;
        }

        ++record_count;
        cur_node += node_size;

        if (record_count == buffer_recs) {
            /* Filled the current buffer */

            /* If buffer not at max size, see if we can grow it */
            if (buffer_recs < buffer_max_recs) {
                uint8_t *old_buf = record_buffer;

                /* add a chunk of records.  if we are near the max,
                 * set the size to the max */
                buffer_recs += buffer_chunk_recs;
                if (buffer_recs + buffer_chunk_recs > buffer_max_recs) {
                    buffer_recs = buffer_max_recs;
                }
                TRACEMSG((("Buffer full--attempt to grow to %" PRIu32
                           " records, %" PRIu32 " bytes"),
                          buffer_recs, node_size * buffer_recs));

                /* attempt to grow */
                record_buffer = (uint8_t*)realloc(record_buffer,
                                                  node_size * buffer_recs);
                if (record_buffer) {
                    /* Success, make certain cur_node points into the
                     * new buffer */
                    cur_node = (record_buffer + (record_count * node_size));
                } else {
                    /* Unable to grow it */
                    TRACEMSG(("realloc() failed"));
                    record_buffer = old_buf;
                    buffer_max_recs = buffer_recs = record_count;
                }
            }

            /* Either buffer at maximum size or attempt to grow it
             * failed. */
            if (record_count == buffer_max_recs) {
                /* Sort */
                skQSort(record_buffer, record_count, node_size, &rwrecCompare);

                /* Write to temp file */
                if (skTempFileWriteBuffer(tmpctx, &temp_file_idx, record_buffer,
                                          node_size, record_count))
                {
                    skAppPrintSyserror("Error writing sorted buffer to"
                                       " temporary file");
                    free(record_buffer);
                    appExit(EXIT_FAILURE);
                }

                /* Reset record buffer to 'empty' */
                record_count = 0;
                cur_node = record_buffer;
            }
        }
    }

    /* Sort (and maybe store) last batch of records */
    if (record_count > 0) {
        skQSort(record_buffer, record_count, node_size, &rwrecCompare);

        if (temp_file_idx >= 0) {
            /* Write last batch to temp file */
            if (skTempFileWriteBuffer(tmpctx, &temp_file_idx, record_buffer,
                                      node_size, record_count))
            {
                skAppPrintSyserror("Error writing sorted buffer to"
                                   " temporary file");
                free(record_buffer);
                appExit(EXIT_FAILURE);
            }
        }
    }

    /* Generate the output */

    if (record_count > 0 && temp_file_idx == -1) {
        /* No temp files written, just output batch of records */
        uint32_t c;

        TRACEMSG((("Writing %" PRIu32 " records to '%s'"),
                  record_count, skStreamGetPathname(out_rwios)));
        for (c = 0, cur_node = record_buffer;
             c < record_count;
             ++c, cur_node += node_size)
        {
            rv = skStreamWriteRecord(out_rwios, (rwRec*)cur_node);
            if (0 != rv) {
                skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
                if (SKSTREAM_ERROR_IS_FATAL(rv)) {
                    free(record_buffer);
                    appExit(EXIT_FAILURE);
                }
            }
        }
    }
    /* else a merge sort is required; which gets invoked from main() */

    if (record_buffer) {
        free(record_buffer);
    }

    return temp_file_idx;
}


int main(int argc, char **argv)
{
    int temp_idx = -1;
    int rv;

    appSetup(argc, argv);                 /* never returns on error */

    if (presorted_input) {
        temp_idx = sortPresorted();
    } else {
        temp_idx = sortRandom();
    }
    if (temp_idx >= 0) {
        mergeFiles(temp_idx);
    }

    if (skStreamGetRecordCount(out_rwios) == 0) {
        /* No records were read at all; write the header to the output
         * file */
        rv = skStreamWriteSilkHeader(out_rwios);
        if (0 != rv) {
            skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
        }
    }

    /* close the file */
    if ((rv = skStreamClose(out_rwios))
        || (rv = skStreamDestroy(&out_rwios)))
    {
        skStreamPrintLastErr(out_rwios, rv, &skAppPrintErr);
        appExit(EXIT_FAILURE);
    }
    out_rwios = NULL;

    appExit(EXIT_SUCCESS);
    return 0; /* NOTREACHED */
}


/*
** Local Variables:
** mode:c
** indent-tabs-mode:nil
** c-basic-offset:4
** End:
*/