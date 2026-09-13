#pragma once
#include <stdint.h>
struct TestFicr { uint32_t CODEPAGESIZE; uint32_t CODESIZE; };
struct TestUicr { uint32_t NRFFW[1]; };
extern TestFicr* NRF_FICR;
extern TestUicr* NRF_UICR;
