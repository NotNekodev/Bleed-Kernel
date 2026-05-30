#pragma once

#define SMAP_ALLOW for (int _i = (stac(), 0); !_i; clac(), _i++)

int SMAP_init(void);

static inline void stac(void){
    asm volatile("stac" ::: "cc");
}

static inline void clac(void){
    asm volatile("clac" ::: "cc");
}