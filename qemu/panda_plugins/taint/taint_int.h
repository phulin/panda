
#ifndef __TAINT_INT_H_
#define __TAINT_INT_H_

#include "panda_memlog.h"
#include "taint_processor.h"

DynValBuffer *create_dynval_buffer(uint32_t size);

void delete_dynval_buffer(DynValBuffer *dynval_buf);                                                             
          
                         

// turns on taint
void taint_enable_taint(void);

// returns 1 if taint is on
int taint_enabled(void);

// label this phys addr in memory with label l
void taint_label_ram(uint64_t pa, uint32_t l);


// if phys addr pa is untainted, return 0.
// else returns label set cardinality 
uint32_t taint_query_ram(uint64_t pa);

// if offset of reg is untainted, ...
uint32_t taint_query_reg(int reg_num, int offset);

// delete taint from this phys addr
void taint_delete_ram(uint64_t pa) ;

// returns number of tainted addrs in ram
uint32_t taint_occ_ram(void);

// returns the max ls type (taint compute #) observed so far
uint32_t taint_max_obs_ls_type(void) ;


   
#endif                                                                                   
