/*
** Copyright (C) 2010-2014 by Carnegie Mellon University.
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
**  rwreadonly.c
**
**    Read SiLK Flow records from files listed on the command line.
**    Use a file name of "-" to read records from the standard input.
**
**    This is a test program to that can be used for library timings.
**
*/

#include <silk/silk.h>

RCSIDENT("$SiLK: rwreadonly.c 1c87fe1a22f7 2014-01-06 15:24:30Z mthomas $");

#include <silk/rwrec.h>
#include <silk/skstream.h>
#include <silk/utils.h>

#define PLURAL(s) (((s) == 1) ? "" : "s")


int main(int argc, char **argv)
{
    SILK_FEATURES_DEFINE_STRUCT(features);
    int64_t ticks = 0;
    rwRec rwrec;
    uint64_t rec_count = 0;
    skstream_t *rwio;
    int exit_val = 0;
    int rv;
    unsigned int i;

    skAppRegister(argv[0]);
    skAppVerifyFeatures(&features, NULL);

    for (i = 1; i < (unsigned int)argc; ++i) {
        rwio = NULL;
        if ((rv = skStreamCreate(&rwio, SK_IO_READ, SK_CONTENT_SILK_FLOW))
            || (rv = skStreamBind(rwio, argv[i]))
            || (rv = skStreamOpen(rwio))
            || (rv = skStreamReadSilkHeader(rwio, NULL)))
        {
            skStreamPrintLastErr(rwio, rv, &skAppPrintErr);
            skStreamDestroy(&rwio);
            exit_val = 1;
            break;
        }
        ticks -= clock();
        while ((rv = skStreamReadRecord(rwio, &rwrec)) == SKSTREAM_OK) {
            ++rec_count;
        }
        ticks += clock();
        if (rv != SKSTREAM_ERR_EOF) {
            skStreamPrintLastErr(rwio, rv, &skAppPrintErr);
            skStreamDestroy(&rwio);
            exit_val = 1;
            break;
        }
        skStreamDestroy(&rwio);
    }

    fprintf(stderr, ("%s: Read %" PRIu64 " record%s from %d file%s"
                     " in %.4f seconds\n"),
            skAppName(), rec_count, PLURAL(rec_count),
            (i - 1), PLURAL(i - 1), (double)ticks / CLOCKS_PER_SEC);

    skAppUnregister();
    return exit_val;
}


/*
** Local Variables:
** mode:c
** indent-tabs-mode:nil
** c-basic-offset:4
** End:
*/
