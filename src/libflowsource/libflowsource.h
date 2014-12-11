/*
** Copyright (C) 2004-2014 by Carnegie Mellon University.
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
**  libflowsource.h
**
**    Definitions/Declaration for the libflowsource library and all
**    the possible external flow sources (IPFIX, PDU, etc).
**
*/
#ifndef _LIBFLOWSOURCE_H
#define _LIBFLOWSOURCE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <silk/silk.h>

RCSIDENTVAR(rcsID_LIBFLOWSOURCE_H, "$SiLK: libflowsource.h cd598eff62b9 2014-09-21 19:31:29Z mthomas $");

#include <silk/silk_types.h>
#include <silk/rwrec.h>
#include <silk/probeconf.h>

/**
 *  @file
 *
 *    libflowsource is used by flowcap and rwflowpack to import
 *    NetFlowV5, IPFIX, or NetFlowV9 flow records.
 *
 *    This file is part of libflowsource.
 */


/**
 *    Value for skpcProbeSetLogFlags() that suppresses all log
 *    messages.
 */
#define SOURCE_LOG_NONE         0

/**
 *    Flag for skpcProbeSetLogFlags() that enables log messages about
 *    out of sequence NetFlow v5 packets.
 */
#define SOURCE_LOG_MISSING      (1 << 0)

/**
 *    Flag for skpcProbeSetLogFlags() that enables log messages about
 *    invalid NetFlow v5 packets.
 */
#define SOURCE_LOG_BAD          (1 << 1)

/**
 *    Flag for skpcProbeSetLogFlags() that enables log messages about
 *    the sampling interval used in NetFlow v9/IPFIX.
 */
#define SOURCE_LOG_SAMPLING     (1 << 2)

/**
 *    Flag for skpcProbeSetLogFlags() that enables log messages about
 *    flow records being ignored due to an NetFlow v9/IPFIX firewall
 *    event setting.
 */
#define SOURCE_LOG_FIREWALL     (1 << 3)

/**
 *    Value for skpcProbeSetLogFlags() that enables all log messages.
 */
#define SOURCE_LOG_ALL          0xff



/**
 *    Number of bytes we want to split between socket buffers
 */
#define SOCKETBUFFER_NOMINAL_TOTAL 0x800000 /* 8M */

/**
 *    Environment variable to modify SOCKETBUFFER_NOMINAL_TOTAL
 */
#define SOCKETBUFFER_NOMINAL_TOTAL_ENV "SK_SOCKETBUFFER_TOTAL"

/**
 *    Minimum number of bytes to attempt to allocate to a socket buffer
 */
#define SOCKETBUFFER_MINIMUM       0x20000 /* 128K */

/**
 *    Environment variable to modify SOCKETBUFFER_MINIMUM
 */
#define SOCKETBUFFER_MINIMUM_ENV "SK_SOCKETBUFFER_MINIMUM"


typedef union skFlowSourceParams_un {
    uint32_t    max_pkts;
    const char *path_name;
} skFlowSourceParams_t;


/**
 *    Structure used to report the statistics of packets processed by
 *    a flow source.
 */
typedef struct skFlowSourceStats_st {
    /** Number of processed packets */
    uint64_t    procPkts;
    /** Number of completely bad packets */
    uint64_t    badPkts;
    /** Number of good records processed */
    uint64_t    goodRecs;
    /** Number of records with bad data */
    uint64_t    badRecs;
    /** Number of missing records; NOTE: signed int to allow for out of
     * seq pkts */
    int64_t     missingRecs;
} skFlowSourceStats_t;

/**
 *    Macro to print the statistics.
 */
#define FLOWSOURCE_STATS_INFOMSG(source_name, source_stats)             \
    INFOMSG(("'%s': Pkts %" PRIu64 "/%" PRIu64                          \
             ", Recs %" PRIu64 ", MissRecs %" PRId64                    \
             ", BadRecs %" PRIu64),                                     \
            (source_name),                                              \
            ((source_stats)->procPkts - (source_stats)->badPkts),       \
            (source_stats)->procPkts, (source_stats)->goodRecs,         \
            (source_stats)->missingRecs, (source_stats)->badRecs)



/***  PDU SOURCES  ********************************************************/

/**
 *    A PDU source is a flow record source based on NetFlow v5 PDU
 *    records.  Once created, records can be requested of it via a
 *    pull mechanism.
 */
typedef struct skPDUSource_st skPDUSource_t;


/**
 *    Creates a PDU source based on a skpc_probe_t.
 *
 *    If the source is a network-based probe, this function also
 *    starts the collection process.
 *
 *    When creating a source from a network-based probe, the 'params'
 *    union should have the 'max_pkts' member specify the maximum
 *    number of packets to buffer in memory for this source.
 *
 *    When creating a source from a probe that specifies either a file
 *    or a directory that is polled for files, the 'params' union must
 *    have the 'path_name' specify the full path of the file to
 *    process.
 *
 *    Return the new source, or NULL on error.
 */
skPDUSource_t *
skPDUSourceCreate(
    const skpc_probe_t         *probe,
    const skFlowSourceParams_t *params);


/**
 *    Stops processing of packets.  This will cause a call to any
 *    skPDUSourceGetGeneric() function to stop blocking.  Meant to be
 *    used as a prelude to skPDUSourceDestroy() in threaded code.
 */
void
skPDUSourceStop(
    skPDUSource_t      *source);


/**
 *    Destroys the PDU source.  This will also cause a call to any
 *    skPDUSourceGetGeneric() function to stop blocking.  In threaded
 *    programs, it is best to call skPDUSourceStop() first, wait for
 *    any consumers of the packets to notice that the source has
 *    stopped, then call skPDUSourceDestroy().
 */
void
skPDUSourceDestroy(
    skPDUSource_t      *source);


/**
 *    Requests a SiLK Flow record from the PDU source.  For a
 *    network-based source, this function will block if there are no
 *    records available.  Returns 0 on success, -1 on error.
 */
int
skPDUSourceGetGeneric(
    skPDUSource_t      *source,
    rwRec              *rwrec);


/**
 *    Logs statistics associated with a PDU source.
 */
void
skPDUSourceLogStats(
    skPDUSource_t      *source);


/**
 *    Logs statistics associated with a PDU source, and then clears
 *    the stats.
 */
void
skPDUSourceLogStatsAndClear(
    skPDUSource_t      *source);


/**
 *    Clears the current statistics for the PDU source.
 */
void
skPDUSourceClearStats(
    skPDUSource_t      *source);


/***  IPFIX SOURCES  ******************************************************/


/**
 *    Values that represent constants used by the IPFIX standard
 *    and/or CISCO devices to represent firewall events:
 *
 *      firewallEvent is an official IPFIX information element, IE 233
 *
 *      NF_F_FW_EVENT is Cisco IE 40005
 *
 *      NF_F_FW_EXT_EVENT is Cisco IE 33002.
 *
 *    The NF_F_FW_EXT_EVENT provides a subtype for the NF_F_FW_EVENT
 *    type.  See the lengthy comment in skipfix.c.
 */
#define SKIPFIX_FW_EVENT_CREATED            1
#define SKIPFIX_FW_EVENT_DELETED            2
#define SKIPFIX_FW_EVENT_DENIED             3
/* denied due to ingress acl */
#define SKIPFIX_FW_EVENT_DENIED_INGRESS       1001
/* denied due to egress acl */
#define SKIPFIX_FW_EVENT_DENIED_EGRESS        1002
/* denied due to attempting to contact ASA's service port */
#define SKIPFIX_FW_EVENT_DENIED_SERV_PORT     1003
/* denied due to first packet not syn */
#define SKIPFIX_FW_EVENT_DENIED_NOT_SYN       1004
#define SKIPFIX_FW_EVENT_ALERT              4
#define SKIPFIX_FW_EVENT_UPDATED            5

/**
 *    Return true if value in 'sfedcv_val' is recognized as a
 *    NF_F_FW_EXT_EVENT sub-value for "Denied" firewall events.
 */
#define SKIPFIX_FW_EVENT_DENIED_CHECK_VALID(sfedcv_val)         \
    (SKIPFIX_FW_EVENT_DENIED_INGRESS <= (sfedcv_val)            \
     && SKIPFIX_FW_EVENT_DENIED_NOT_SYN >= (sfedcv_val))


/**
 *    An IPFIX source is a flow record source based on IPFIX or
 *    NetFlow V9 records.  Once created, records can be requested of
 *    it via a pull mechanism.
 */
typedef struct skIPFIXSource_st skIPFIXSource_t;


/**
 *    Record type returned by skIPFIXSource_t.
 */
typedef rwGenericRec_V5 skIPFIXSourceRecord_t;


/**
 *    Get a pointer to a SiLK Flow record given a pointer to an
 *    skIPFIXSourceRecord_t.
 */
#define skIPFIXSourceRecordGetRwrec(ipfix_src_rec)      \
    ((rwRec*)(ipfix_src_rec))


/**
 *    Performs any initialization required prior to creating the IPFIX
 *    sources.  Returns 0 on success, or -1 on failure.
 */
int
skIPFIXSourcesSetup(
    void);


/**
 *    Creates a IPFIX source based on an skpc_probe_t.
 *
 *    If the source is a network-based probe, this function also
 *    starts the collection process.
 *
 *    When creating a source from a network-based probe, the 'params'
 *    union should have the 'max_pkts' member specify the maximum
 *    number of packets to buffer in memory for this source.
 *
 *    When creating a source from a probe that specifies either a file
 *    or a directory that is polled for files, the 'params' union must
 *    have the 'path_name' specify the full path of the file to
 *    process.
 *
 *    Return the new source, or NULL on error.
 */
skIPFIXSource_t *
skIPFIXSourceCreate(
    const skpc_probe_t         *probe,
    const skFlowSourceParams_t *params);


/**
 *    Stops processing of packets.  This will cause a call to any
 *    skIPFIXSourceGetGeneric() function to stop blocking.  Meant to
 *    be used as a prelude to skIPFIXSourceDestroy() in threaded code.
 */
void
skIPFIXSourceStop(
    skIPFIXSource_t    *source);


/**
 *    Destroys a IPFIX source.
 */
void
skIPFIXSourceDestroy(
    skIPFIXSource_t    *source);


/**
 *    Requests a SiLK Flow record from the IPFIX source 'source'.
 *
 *    This function will block if there are no IPFIX flows available
 *    from which to create a SiLK Flow record.
 *
 *    Returns 0 on success, -1 on failure.
 */
int
skIPFIXSourceGetGeneric(
    skIPFIXSource_t    *source,
    rwRec              *rwrec);

/**
 *    Requests a record from the IPFIX source 'source'.
 *
 *    This function will block if there are no IPFIX flows available
 *    from which to create a record.
 *
 *    Returns 0 on success, -1 on failure.
 */
int
skIPFIXSourceGetRecord(
    skIPFIXSource_t        *source,
    skIPFIXSourceRecord_t  *ipfix_rec);

/**
 *    Logs statistics associated with a IPFIX source.
 */
void
skIPFIXSourceLogStats(
    skIPFIXSource_t    *source);


/**
 *    Logs statistics associated with a IPFIX source, and then clears
 *    the stats.
 */
void
skIPFIXSourceLogStatsAndClear(
    skIPFIXSource_t    *source);


/**
 *    Clears the current statistics for the IPFIX source.
 */
void
skIPFIXSourceClearStats(
    skIPFIXSource_t    *source);



#ifdef __cplusplus
}
#endif
#endif /* _LIBFLOWSOURCE_H */

/*
** Local Variables:
** mode:c
** indent-tabs-mode:nil
** c-basic-offset:4
** End:
*/
