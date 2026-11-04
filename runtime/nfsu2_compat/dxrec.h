/* D3DX HLE stage 2: a record of the IDirect3DDevice9 calls a guest
 * function makes, and the "would it be a no-op if done now" check.
 * The record is kept in com.c (the device state copies live there); the
 * user is runtime/d3dx_hle.c.                                           */
#ifndef DXREC_H
#define DXREC_H
#include <stdint.h>


typedef struct {
    int16_t  mid;            /* com.c method id */
    uint16_t doff, dlen;     /* constant data inside data[] */
    uint8_t  red;            /* was a no-op when recorded */
    uint32_t a[3];           /* first 3 arguments after this */
} DxCall;

/* Capacity per record: a state record holds 8 calls / 256 bytes, a pass
 * record more.  The call/data arrays live in the struct that carries the
 * record; DXREC_INIT wires them up. */
typedef struct {
    uint8_t  kind;           /* DXREC_PASS / DXREC_STATE */
    uint8_t  ok;             /* 0: cannot be memoized (unsupported call, nested, overflow, state block) */
    uint8_t  allred;         /* every call was a no-op while recording */
    uint16_t ncall, maxcall;
    uint16_t ndata, maxdata;
    DxCall  *call;
    uint8_t *data;
} DxRec;
#define DXREC_INIT(r, calls, databuf) do{ (r)->call = (calls); (r)->data = (databuf); \
    (r)->maxcall = (uint16_t)(sizeof(calls)/sizeof((calls)[0])); \
    (r)->maxdata = (uint16_t)sizeof(databuf); (r)->ok = 0; (r)->ncall = (r)->ndata = 0; }while(0)

#define DXREC_PASS  1
#define DXREC_STATE 2
/* A state record inside a pass is normal.  If a new record starts inside a
 * STATE record (nested apply = compound), ALL outer records become
 * invalid; a nested record of the same kind also invalidates the outer one. */
void dxrec_begin(DxRec *r, int kind);
void dxrec_append(const DxRec *src); /* add a skipped state's recorded calls to the open records */
void dxrec_end(void);
int  dxrec_redundant(const DxRec *r);/* is every recorded call a no-op NOW */
int  dxrec_sb_recording(void);
const char *dxrec_mid_name(int mid);       /* is a state block recording open (no skipping) */

#endif
