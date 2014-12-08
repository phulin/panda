/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <sys/mman.h>

#include "defines.h"
#include "label_set.h"
#include "fast_shad.h"

FastShad *fast_shad_new(uint64_t labelsets) {
    FastShad *result = (FastShad *)malloc(sizeof(FastShad));
    uint64_t size = sizeof(LabelSet *) * labelsets;
    if (!result) return NULL;

    LabelSet **array;
    if (labelsets < (1UL << 24)) {
        array = (LabelSet **)malloc(size);
        printf("taint2: Allocating small fast_shad (%" PRIu64 " bytes) using malloc @ %lx.\n",
                size, (uint64_t)array);
        assert(array);
    } else {
        uint64_t align = 1UL << 40; // Align to a 1T boundary.
        assert(align > size);
        uint64_t vaddr = 0;
        do {
            // We're going to try to make this aligned.
            vaddr += align;
            printf("taint2: Trying to allocate large fast_shad @ 0x%" PRIx64 ".\n", vaddr);
            array = (LabelSet **)mmap((void *)vaddr, size, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED | MAP_HUGETLB,
                    -1, 0);
            if (array == (LabelSet **)MAP_FAILED) {
                printf("taint2: Hugetlb failed. Trying without.\n");
                // try without HUGETLB
                array = (LabelSet **)mmap((void *)vaddr, size, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
            }
        } while (array == (LabelSet **)MAP_FAILED && vaddr <= align * 8); // only try 8 times.
        if (array == (LabelSet **)MAP_FAILED) {
            puts(strerror(errno));
            return NULL;
        }
    }

    result->labels = array;
    result->orig_labels = array;
    result->size = labelsets;

    return result;
}

// release all memory associated with this fast_shad.
void fast_shad_free(FastShad *fast_shad) {
    if (fast_shad->size < (1UL << 24)) {
        free(fast_shad->labels);
    } else {
        munmap(fast_shad->labels, sizeof(LabelSet *) * fast_shad->size);
    }
    free(fast_shad);
}
