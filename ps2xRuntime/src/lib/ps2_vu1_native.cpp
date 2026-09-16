#include "runtime/ps2_vu1_native.h"
#include "runtime/ps2_vu1.h"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>

namespace vu1native
{
    namespace
    {
        Ctx g_ctx;

        // ---- VU arithmetic helpers, mirroring vu1_jit_ops.inc exactly ----------------------
        inline __m128 bc(__m128 v, int lane)
        {
            switch (lane)
            {
            case 0: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
            case 1: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
            case 2: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
            default: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
            }
        }
        // MULAx ACC,r0,v.x ; MADDAy ACC,r1,v.y ; MADDAz ACC,r2,v.z ; MADDw out,r3,vf00.w (=1.0).
        // Two roundings per MADD (mul then add), never fused -- same as the recompiled code.
        inline __m128 xformAcc(const __m128 *r, __m128 v, __m128 &acc)
        {
            acc = _mm_mul_ps(r[0], bc(v, 0));
            acc = _mm_add_ps(acc, _mm_mul_ps(r[1], bc(v, 1)));
            acc = _mm_add_ps(acc, _mm_mul_ps(r[2], bc(v, 2)));
            return _mm_add_ps(acc, _mm_mul_ps(r[3], _mm_set1_ps(1.0f)));
        }
        inline float vuClampFloat(float f)
        {
            uint32_t u; std::memcpy(&u, &f, 4);
            if ((u & 0x7F800000u) == 0x7F800000u) { u = (u & 0x80000000u) | 0x7F7FFFFFu; std::memcpy(&f, &u, 4); }
            return f;
        }
        // FTOI0/FTOI4 as the jit does them: cvttps, then the 0x80000000 "invalid" lanes become
        // INT32_MIN / INT32_MAX by the input's sign.
        inline __m128i ftoi(__m128 f)
        {
            __m128i iv = _mm_cvttps_epi32(f);
            const __m128i bad = _mm_cmpeq_epi32(iv, _mm_set1_epi32((int)0x80000000));
            if (_mm_movemask_epi8(bad))
            {
                const __m128i neg = _mm_srai_epi32(_mm_castps_si128(f), 31);
                const __m128i satv = _mm_or_si128(_mm_and_si128(neg, _mm_set1_epi32((int)0x80000000)),
                                                  _mm_andnot_si128(neg, _mm_set1_epi32(0x7FFFFFFF)));
                iv = _mm_or_si128(_mm_and_si128(bad, satv), _mm_andnot_si128(bad, iv));
            }
            return iv;
        }
        inline __m128i ftoi4(__m128 f) { return ftoi(_mm_mul_ps(f, _mm_set1_ps(16.0f))); }
        // CLIPw.xyz v, v: six ordered compares against |w|, PS2 flag order x+ x- y+ y- z+ z-.
        inline uint32_t clipFlags(__m128 v)
        {
            alignas(16) float c[4]; _mm_store_ps(c, v);
            const float w = std::fabs(c[3]);
            uint32_t f = 0;
            if (c[0] > w) f |= 0x01; if (c[0] < -w) f |= 0x02;
            if (c[1] > w) f |= 0x04; if (c[1] < -w) f |= 0x08;
            if (c[2] > w) f |= 0x10; if (c[2] < -w) f |= 0x20;
            return f;
        }
        inline __m128 ldq(const uint8_t *vuData, uint32_t dataSize, uint32_t qw)
        {
            return _mm_loadu_ps(reinterpret_cast<const float *>(vuData + ((qw * 16u) & (dataSize - 1u))));
        }
        inline void stq(uint8_t *vuData, uint32_t dataSize, uint32_t qw, __m128 v)
        {
            _mm_storeu_ps(reinterpret_cast<float *>(vuData + ((qw * 16u) & (dataSize - 1u))), v);
        }
        inline void stqi(uint8_t *vuData, uint32_t dataSize, uint32_t qw, __m128i v)
        {
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuData + ((qw * 16u) & (dataSize - 1u))), v);
        }
        // dest-mask .xyz: take xyz from b, keep a's w (SSE2, no dependency on SSE4.1 blend).
        inline __m128 xyzOf(__m128 a, __m128 b)
        {
            const __m128 m = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
            return _mm_or_ps(_mm_and_ps(m, b), _mm_andnot_ps(m, a));
        }
        inline __m128 vf(const VU1State &st, int r) { return _mm_loadu_ps(st.vf[r]); }
        inline void setvf(VU1State &st, int r, __m128 v) { _mm_storeu_ps(st.vf[r], v); }
        inline int16_t i16(int32_t v) { return (int16_t)v; }

        // ---- 3b5dfe97: two-bone skinned tristrip, textured pass + toon-ramp pass -------------
        // Listing: work/vu1dis_3b5dfe97.txt. Model: work/rig/vu1ref_3b5dfe97.py. Batch layout at
        // TOP: [0] A+D giftag, [1] TEX0 (pass A), [2] TEX0 (pass B), [3] geometry tag A (x = count),
        // [4] geometry tag B; vertices from TOP+5, 3 qw each: P (w = weight), N, T. Output at
        // TOP+5+252: pass A header + count*3, pass B header + count*3; one XGKICK covers both.
        constexpr uint64_t kHash3b5dfe97 = 0x0492d545dc59311dull;
        Fn g_generic3b5dfe97 = nullptr;

        void kernel3b5dfe97(VU1Interpreter &vu, VU1State &st, uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory, uint32_t maxCycles)
        {
            const uint32_t entryPc = st.pc;
            const uint32_t top = st.top & 0x3FFu;
            const uint32_t countRaw = *reinterpret_cast<const uint32_t *>(vuData + (((top + 3u) * 16u) & (dataSize - 1u)));
            const int32_t count = (int32_t)(countRaw & 0x7FFFu);
            if ((entryPc != 0x120u && entryPc != 0x3e8u) || count <= 0 || dataSize < 16384u)
            {
                g_generic3b5dfe97(vu, st, vuData, dataSize, gs, memory, maxCycles);
                return;
            }

            // Constants (LQ n(vi00)).
            __m128 A[4], B[4], C[4], D[4], E[4], F[4];
            for (int i = 0; i < 4; ++i)
            {
                A[i] = ldq(vuData, dataSize, 0u + i);  B[i] = ldq(vuData, dataSize, 4u + i);
                C[i] = ldq(vuData, dataSize, 26u + i); D[i] = ldq(vuData, dataSize, 30u + i);
                E[i] = ldq(vuData, dataSize, 14u + i); F[i] = ldq(vuData, dataSize, 18u + i);
            }
            const __m128 c8 = ldq(vuData, dataSize, 8u), c9 = ldq(vuData, dataSize, 9u);
            const __m128 qw22 = ldq(vuData, dataSize, 22u);
            const __m128i rgbaA = ftoi(qw22);                  // vf23 = FTOI0(vf20 = qw22)
            const __m128 vf18 = vf(st, 18);                    // pass-B RGBAQ, left by the setup run
            const __m128 vf30 = vf(st, 30), vf31 = vf(st, 31);
            const float vf22w = st.vf[22][3];                  // never written in the loop
            const __m128 half = _mm_set1_ps(0.5f);

            // Header copy (vf17, vf27, vf20, vf25, vf26 = TOP+0..4).
            const __m128 h0 = ldq(vuData, dataSize, top + 0u), h1 = ldq(vuData, dataSize, top + 1u),
                         h2 = ldq(vuData, dataSize, top + 2u), h3 = ldq(vuData, dataSize, top + 3u),
                         h4 = ldq(vuData, dataSize, top + 4u);
            // Integer registers, 16-bit like the VU.
            const int16_t vi03Hdr = i16((int32_t)top + 5);           // after the LQI/ILWR header walk
            const int16_t vi02 = i16(vi03Hdr + 252);
            int16_t vi07 = i16(vi02 + 3);
            int16_t vi04 = i16(vi07 + count * 3);                    // IADD x3
            stq(vuData, dataSize, (uint16_t)vi02 + 0u, h0); stq(vuData, dataSize, (uint16_t)vi02 + 1u, h1); stq(vuData, dataSize, (uint16_t)vi02 + 2u, h3);
            stq(vuData, dataSize, (uint16_t)vi04 + 0u, h0); stq(vuData, dataSize, (uint16_t)vi04 + 1u, h2); stq(vuData, dataSize, (uint16_t)vi04 + 2u, h4);
            vi04 = i16(vi04 + 3);
            int16_t vi03 = vi03Hdr;

            uint32_t clip = (*g_ctx.clipWait > 0u) ? *g_ctx.pendingClip : st.clip;
            uint64_t cyc = (entryPc == 0x3e8u ? 2u : 0u) + 26u;
            __m128 acc = _mm_setzero_ps();
            __m128 P = ldq(vuData, dataSize, (uint16_t)vi03), N, T, vf28, vf29, vf20, vf24, vf25, vf26, vf27, vf17, vf22;
            __m128i vf29i = _mm_setzero_si128();
            float q = st.q;
            int vi01 = 0;
            // Prologue for vertex 0 (0x178-0x1e8), then the loop (0x1f0-0x3c0) whose tail computes the
            // next vertex's vf28/vf29/vf28' -- the software pipeline; the dataflow per vertex is the same.
            vf28 = _mm_sub_ps(P, vf30); vf29 = _mm_sub_ps(P, vf31);
            vf28 = xformAcc(A, vf28, acc);
            for (int32_t v = 0; v < count; ++v)
            {
                N = ldq(vuData, dataSize, (uint16_t)vi03 + 1u);
                vf29 = xformAcc(B, vf29, acc);
                vf26 = xformAcc(C, N, acc);
                vf20 = _mm_sub_ps(vf28, vf29);
                vf27 = xformAcc(D, N, acc);
                vf20 = _mm_mul_ps(vf20, bc(P, 3));
                vf20 = _mm_add_ps(vf29, vf20);
                vf26 = _mm_sub_ps(vf26, vf27);
                vf24 = xformAcc(E, vf20, acc);
                vf26 = _mm_mul_ps(vf26, bc(P, 3));
                vf25 = xformAcc(F, vf20, acc);
                {   // DIV Q, vf00w, vf24w
                    alignas(16) float t[4]; _mm_store_ps(t, vf24);
                    const float den = t[3];
                    q = (den != 0.0f) ? vuClampFloat(1.0f / den) : FLT_MAX;
                }
                T = ldq(vuData, dataSize, (uint16_t)vi03 + 2u);
                vf26 = _mm_add_ps(vf27, vf26);
                clip = ((clip << 6) | clipFlags(vf25)) & 0xFFFFFFu;
                const __m128 qv = _mm_set1_ps(q);
                {   // MULi.xyz vf26 (w untouched)
                    const __m128 m = _mm_mul_ps(vf26, half);
                    vf26 = xyzOf(vf26, m);
                }
                vf24 = _mm_mul_ps(vf24, qv);                                        // MULq.xyzw
                vf17 = xyzOf(T, _mm_mul_ps(T, qv));                    // MULq.xyz vf17 = T*Q
                vf26 = xyzOf(vf26, _mm_add_ps(vf26, half));             // ADDi.xyz
                vf29i = ftoi4(vf24);                                                // FTOI4 vf29
                vi01 = (clip & 0x3FFFFu) != 0u ? 1 : 0;                             // FCAND
                stq(vuData, dataSize, (uint16_t)vi07 + 0u, vf17);                   // SQ vf17, 0(vi07)
                vi03 = i16(vi03 + 3);
                P = ldq(vuData, dataSize, (uint16_t)vi03);                          // LQ vf19, 0(vi03) (next / stale)
                {   // ADD.x vf22 = vf00.x + vf26.x ; ADD.y = 0 ; ADDw.z = vf00.w (=1)
                    alignas(16) float s[4]; _mm_store_ps(s, vf26);
                    vf22 = _mm_set_ps(vf22w, 0.0f + 1.0f, 0.0f + 0.0f, 0.0f + s[0]);
                }
                vf28 = _mm_sub_ps(P, vf30);
                stqi(vuData, dataSize, (uint16_t)vi07 + 2u, vf29i);                 // SQ vf29, 2(vi07)
                vf22 = xyzOf(vf22, _mm_mul_ps(vf22, qv));              // MULq.xyz vf22
                stqi(vuData, dataSize, (uint16_t)vi04 + 2u, vf29i);                 // SQ vf29, 2(vi04)
                vf29 = _mm_sub_ps(P, vf31);
                stq(vuData, dataSize, (uint16_t)vi04 + 1u, vf18);                   // SQ vf18, 1(vi04)
                // Tail: next vertex's A-transform (0x378-0x3a0), pointer bumps, colour + B-ST stores.
                acc = _mm_mul_ps(A[0], bc(vf28, 0));
                acc = _mm_add_ps(acc, _mm_mul_ps(A[1], bc(vf28, 1)));
                vi07 = i16(vi07 + 3);
                vi04 = i16(vi04 + 3);
                acc = _mm_add_ps(acc, _mm_mul_ps(A[2], bc(vf28, 2)));
                stqi(vuData, dataSize, (uint16_t)(int16_t)(vi07 - 2), rgbaA);       // SQ vf23, -2(vi07)
                stq(vuData, dataSize, (uint16_t)(int16_t)(vi04 - 3), vf22);         // SQ vf22, -3(vi04)
                vf28 = _mm_add_ps(acc, _mm_mul_ps(A[3], _mm_set1_ps(1.0f)));        // MADDw vf28
                if (vi01)
                {   // ISW.w vi13 (0x8000) into the XYZF words: fog 0, ADC 1
                    const uint32_t adc = 0x8000u;
                    std::memcpy(vuData + ((((uint16_t)(int16_t)(vi07 - 1)) * 16u + 12u) & (dataSize - 1u)), &adc, 4);
                    std::memcpy(vuData + ((((uint16_t)(int16_t)(vi04 - 1)) * 16u + 12u) & (dataSize - 1u)), &adc, 4);
                    cyc += (v + 1 < count) ? 60u : 63u;
                }
                else
                    cyc += (v + 1 < count) ? 58u : 61u;
            }

            // Register state as the recompiled loop leaves it (VERIFY compares all of it).
            for (int i = 0; i < 4; ++i) { setvf(st, 1 + i, A[i]); setvf(st, 5 + i, B[i]); setvf(st, 9 + i, C[i]); setvf(st, 13 + i, D[i]); }
            setvf(st, 17, vf17); setvf(st, 19, P); setvf(st, 20, qw22); setvf(st, 21, N); setvf(st, 22, vf22);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(st.vf[23]), rgbaA);
            setvf(st, 24, vf24); setvf(st, 25, vf25); setvf(st, 26, vf26); setvf(st, 27, vf27);
            setvf(st, 28, vf28); setvf(st, 29, vf29);   // vf29: the tail's SUB (stale P - c9) lands after the FTOI4
            _mm_storeu_ps(st.acc, acc);
            st.vi[1] = vi01; st.vi[2] = vi02; st.vi[3] = vi03; st.vi[4] = vi04; st.vi[7] = vi07;
            st.vi[10] = 0; st.vi[11] = 0x7FFF;
            st.i = 0.5f;
            st.q = q; st.pendingQ = q; st.qWait = 0u;
            st.clip = clip; *g_ctx.pendingClip = clip; *g_ctx.clipWait = 0u;
            st.pc = 0x3e8u; st.ebit = true;
            if (g_ctx.pairCount) g_ctx.pairCount->fetch_add(cyc + 4u, std::memory_order_relaxed);
            vu.xgkickImpl(2u, vuData, dataSize, gs, memory);   // XGKICK vi02
        }
        // ---- 925edd7c: single-pass two-bone skinned tristrip ---------------------------------
        // Listing: work/vu1dis_925edd7c.txt. Model: work/rig/vu1ref_925edd7c.py. Header at TOP:
        // [0] A+D giftag, [1] TEX0, [2..3] unused, [4] geometry tag (x = count); vertices from
        // TOP+5: P (w = weight), unused, T. Output at TOP+5+252: header + count*3 of
        // [ST*Q, vf09 (colour left by the setup pass), FTOI4]. No ADC marking (the FCAND branch
        // only skips a NOP). Entries 0x80 / 0x318 (the B 0x80 stub).
        constexpr uint64_t kHash925edd7c = 0x7e8efb7575b22f69ull;
        Fn g_generic925edd7c = nullptr;

        void kernel925edd7c(VU1Interpreter &vu, VU1State &st, uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory, uint32_t maxCycles)
        {
            const uint32_t entryPc = st.pc;
            const uint32_t top = st.top & 0x3FFu;
            const uint32_t countRaw = *reinterpret_cast<const uint32_t *>(vuData + (((top + 4u) * 16u) & (dataSize - 1u)));
            const int32_t count = (int32_t)(countRaw & 0x7FFFu);
            if ((entryPc != 0x80u && entryPc != 0x318u) || count <= 0 || dataSize < 16384u)
            {
                g_generic925edd7c(vu, st, vuData, dataSize, gs, memory, maxCycles);
                return;
            }
            __m128 A[4], B[4], E[4], F[4];
            for (int i = 0; i < 4; ++i)
            {
                A[i] = ldq(vuData, dataSize, 0u + i);  B[i] = ldq(vuData, dataSize, 4u + i);
                E[i] = ldq(vuData, dataSize, 11u + i); F[i] = ldq(vuData, dataSize, 15u + i);
            }
            const __m128 vf30 = vf(st, 30), vf31 = vf(st, 31), vf09 = vf(st, 9);
            const __m128 h0 = ldq(vuData, dataSize, top + 0u), h1 = ldq(vuData, dataSize, top + 1u),
                         h4 = ldq(vuData, dataSize, top + 4u);
            const int16_t vi03Hdr = i16((int32_t)top + 5);
            const int16_t vi02 = i16(vi03Hdr + 252);
            int16_t vi07 = i16(vi02 + 3);
            int16_t vi03 = vi03Hdr;
            stq(vuData, dataSize, (uint16_t)vi02 + 0u, h0); stq(vuData, dataSize, (uint16_t)vi02 + 1u, h1); stq(vuData, dataSize, (uint16_t)vi02 + 2u, h4);

            uint32_t clip = (*g_ctx.clipWait > 0u) ? *g_ctx.pendingClip : st.clip;
            uint64_t cyc = (entryPc == 0x318u ? 2u : 0u) + 24u;
            __m128 acc = _mm_setzero_ps();
            __m128 P = ldq(vuData, dataSize, (uint16_t)vi03), N = _mm_setzero_ps(), T, vf28, vf29, vf20 = _mm_setzero_ps(), vf24 = _mm_setzero_ps(), vf25 = _mm_setzero_ps(), vf22 = _mm_setzero_ps();
            __m128i vf29i;
            float q = st.q;
            int vi01 = 0;
            vf28 = _mm_sub_ps(P, vf30); vf29 = _mm_sub_ps(P, vf31);
            vf28 = xformAcc(A, vf28, acc);
            for (int32_t v = 0; v < count; ++v)
            {
                N = ldq(vuData, dataSize, (uint16_t)vi03 + 1u);                     // LQ vf21 (unused)
                vf29 = xformAcc(B, vf29, acc);
                vf20 = _mm_sub_ps(vf28, vf29);
                vf20 = _mm_mul_ps(vf20, bc(P, 3));
                vf20 = _mm_add_ps(vf29, vf20);
                vf24 = xformAcc(E, vf20, acc);
                vf25 = xformAcc(F, vf20, acc);
                {
                    alignas(16) float t[4]; _mm_store_ps(t, vf24);
                    const float den = t[3];
                    q = (den != 0.0f) ? vuClampFloat(1.0f / den) : FLT_MAX;
                }
                T = ldq(vuData, dataSize, (uint16_t)vi03 + 2u);
                clip = ((clip << 6) | clipFlags(vf25)) & 0xFFFFFFu;
                const __m128 qv = _mm_set1_ps(q);
                vf24 = _mm_mul_ps(vf24, qv);
                vf22 = xyzOf(T, _mm_mul_ps(T, qv));
                vi01 = (clip & 0x3FFFFu) != 0u ? 1 : 0;
                stq(vuData, dataSize, (uint16_t)vi07 + 0u, vf22);                   // SQ vf22, 0(vi07)
                vi03 = i16(vi03 + 3);
                vf29i = ftoi4(vf24);
                P = ldq(vuData, dataSize, (uint16_t)vi03);                          // LQ vf19 (next / stale)
                vf28 = _mm_sub_ps(P, vf30);
                stqi(vuData, dataSize, (uint16_t)vi07 + 2u, vf29i);                 // SQ vf29, 2(vi07)
                vf29 = _mm_sub_ps(P, vf31);
                acc = _mm_mul_ps(A[0], bc(vf28, 0));
                acc = _mm_add_ps(acc, _mm_mul_ps(A[1], bc(vf28, 1)));
                vi07 = i16(vi07 + 3);
                acc = _mm_add_ps(acc, _mm_mul_ps(A[2], bc(vf28, 2)));
                stq(vuData, dataSize, (uint16_t)(int16_t)(vi07 - 2), vf09);         // SQ vf09, -2(vi07)
                vf28 = _mm_add_ps(acc, _mm_mul_ps(A[3], _mm_set1_ps(1.0f)));
                cyc += (v + 1 < count) ? 56u : 59u;
            }
            for (int i = 0; i < 4; ++i) { setvf(st, 1 + i, A[i]); setvf(st, 5 + i, B[i]); }
            setvf(st, 19, P); setvf(st, 20, vf20); setvf(st, 21, N); setvf(st, 22, vf22);
            setvf(st, 24, vf24); setvf(st, 25, vf25); setvf(st, 26, h0); setvf(st, 27, h1);   // the header LQIs survive the loop here
            setvf(st, 28, vf28); setvf(st, 29, vf29);
            _mm_storeu_ps(st.acc, acc);
            st.vi[1] = vi01; st.vi[2] = vi02; st.vi[3] = vi03; st.vi[7] = vi07;
            st.vi[10] = 0; st.vi[11] = 0x7FFF;
            st.q = q; st.pendingQ = q; st.qWait = 0u;
            st.clip = clip; *g_ctx.pendingClip = clip; *g_ctx.clipWait = 0u;
            st.pc = 0x318u; st.ebit = true;
            if (g_ctx.pairCount) g_ctx.pairCount->fetch_add(cyc, std::memory_order_relaxed);
            vu.xgkickImpl(2u, vuData, dataSize, gs, memory);
        }
    } // namespace

    void bind(const Ctx &ctx) { g_ctx = ctx; }

    bool enabled()
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_VUNATIVE"); return v && v[0] && v[0] != '0'; }();
        return s_on;
    }

    Fn lookup(uint64_t hash)
    {
        if (hash == kHash3b5dfe97) return &kernel3b5dfe97;
        if (hash == kHash925edd7c) return &kernel925edd7c;
        return nullptr;
    }

    void setGeneric(uint64_t hash, Fn generic)
    {
        if (hash == kHash3b5dfe97) g_generic3b5dfe97 = generic;
        if (hash == kHash925edd7c) g_generic925edd7c = generic;
    }
}
