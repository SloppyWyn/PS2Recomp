#include "Common.h"
#include "VU.h"

#include <limits>

namespace ps2_stubs
{

    namespace
    {
        bool readVuVec4f(uint8_t *rdram, uint32_t addr, float (&out)[4])
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(out, ptr, sizeof(out));
            return true;
        }

        bool writeVuVec4f(uint8_t *rdram, uint32_t addr, const float (&in)[4])
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, in, sizeof(in));
            return true;
        }

        bool readVuVec4i(uint8_t *rdram, uint32_t addr, int32_t (&out)[4])
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(out, ptr, sizeof(out));
            return true;
        }

        bool writeVuVec4i(uint8_t *rdram, uint32_t addr, const int32_t (&in)[4])
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, in, sizeof(in));
            return true;
        }

        bool readVuMatrix4f(uint8_t *rdram, uint32_t addr, float (&out)[16])
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(out, ptr, sizeof(out));
            return true;
        }

        bool writeVuMatrix4f(uint8_t *rdram, uint32_t addr, const float (&in)[16])
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, in, sizeof(in));
            return true;
        }

        // Row-major matrix product: out = lhs * rhs (lhs is the left factor).
        void mulVuMatrix(const float (&lhs)[16], const float (&rhs)[16], float (&out)[16])
        {
            std::fill(std::begin(out), std::end(out), 0.0f);
            for (int i = 0; i < 4; ++i)
            {
                for (int j = 0; j < 4; ++j)
                {
                    for (int k = 0; k < 4; ++k)
                    {
                        out[4 * i + j] += rhs[4 * k + j] * lhs[4 * i + k];
                    }
                }
            }
        }

        // A float as the VU leaves it: Inf/NaN -> +-MAX, denormal -> +-0.
        float vuClamp(float value)
        {
            uint32_t ignored = 0u;
            return _mm_cvtss_f32(ps2x_vu0::normalizeResult(_mm_set1_ps(value), ignored));
        }

        // Q of "vdiv Q, num, den": +-MAX when den is 0.
        float vuDivide(float num, float den)
        {
            if (den == 0.0f)
                return (std::signbit(num) != std::signbit(den)) ? -std::numeric_limits<float>::max()
                                                                 : std::numeric_limits<float>::max();
            return vuClamp(num / den);
        }

        // libvu0 sceVu0Normalize: xyz scaled by 1/|xyz| (vsqrt, vdiv), w = 0.
        void normalizeXyz(const float (&s)[4], float (&o)[4])
        {
            const float len = std::sqrt((s[0] * s[0]) + (s[1] * s[1]) + (s[2] * s[2]));
            const float q = vuDivide(1.0f, len);
            for (int i = 0; i < 3; ++i)
                o[i] = vuClamp(s[i] * q);
            o[3] = 0.0f;
        }

        // FTOI0/FTOI4 as the VU does them: truncating, saturating.
        int32_t vuFtoi(float value, double scale)
        {
            return ps2x_vu0::floatToInt(vuClamp(value), scale);
        }

        void makeIdentityMatrix(float (&out)[16])
        {
            std::fill(std::begin(out), std::end(out), 0.0f);
            out[0] = 1.0f;
            out[5] = 1.0f;
            out[10] = 1.0f;
            out[15] = 1.0f;
        }

        // Rigid-transform inverse under the file's row-vector convention
        // (translation in row 3, ApplyMatrix computes v*M): transpose the 3x3
        // rotation block, zero its 4th column, and set the translation row to
        // -t*R^T -- each lane dots t with a ROW of R: out[12+col] =
        // -(t . R[col]). This makes M * rigidInverse(M) == I. Copy [15].
        void rigidInverse(const float (&in)[16], float (&out)[16])
        {
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                    out[4 * row + col] = in[4 * col + row];
                out[4 * row + 3] = 0.0f;
            }
            const float tx = in[12], ty = in[13], tz = in[14];
            for (int col = 0; col < 3; ++col)
                out[12 + col] = -((tx * in[4 * col]) + (ty * in[4 * col + 1]) + (tz * in[4 * col + 2]));
            out[15] = in[15];
        }

        void axisRotateMatrix(const float (&in)[16], float angle, int axis, float (&out)[16])
        {
            float rot[16]{};
            makeIdentityMatrix(rot);
            const float cs = std::cos(angle);
            const float sn = std::sin(angle);
            if (axis == 2)
            {
                rot[0] = cs;
                rot[1] = sn;
                rot[4] = -sn;
                rot[5] = cs;
            }
            else if (axis == 1)
            {
                rot[0] = cs;
                rot[2] = -sn;
                rot[8] = sn;
                rot[10] = cs;
            }
            else
            {
                rot[5] = cs;
                rot[6] = sn;
                rot[9] = -sn;
                rot[10] = cs;
            }
            mulVuMatrix(in, rot, out);
        }

        // libvu0 sceVu0RotTransPers(N): v' = v*m; Q = 1/v'.w (+-MAX for w = 0); xyz *= Q (w kept);
        // FTOI4 on all four fields, and with a non-zero mode (the SDK's "mode" argument) FTOI0 on
        // z and w instead (checked against the game's own copy at 0x10fc60).
        void rotTransPersOne(const float (&m)[16], const float (&v)[4], bool zwFtoi0, int32_t (&out)[4])
        {
            float t[4];
            t[0] = (m[0] * v[0]) + (m[4] * v[1]) + (m[8] * v[2]) + (m[12] * v[3]);
            t[1] = (m[1] * v[0]) + (m[5] * v[1]) + (m[9] * v[2]) + (m[13] * v[3]);
            t[2] = (m[2] * v[0]) + (m[6] * v[1]) + (m[10] * v[2]) + (m[14] * v[3]);
            t[3] = (m[3] * v[0]) + (m[7] * v[1]) + (m[11] * v[2]) + (m[15] * v[3]);
            const float q = vuDivide(1.0f, vuClamp(t[3]));
            t[0] = vuClamp(t[0] * q);
            t[1] = vuClamp(t[1] * q);
            t[2] = vuClamp(t[2] * q);
            out[0] = vuFtoi(t[0], 16.0);
            out[1] = vuFtoi(t[1], 16.0);
            out[2] = vuFtoi(t[2], zwFtoi0 ? 1.0 : 16.0);
            out[3] = vuFtoi(t[3], zwFtoi0 ? 1.0 : 16.0);
        }

        // libvu0 sceVu0ClipScreen: the status flags of "vsub.xyw (v - 0)" and "vsub.xy (4096 - v)"
        // masked with 0xc0: ZS (0x40) when x, y, w or 4096-x, 4096-y is zero, SS (0x80) when one
        // is negative. Non-zero = the vertex is off the 4096x4096 GS drawing area.
        int32_t screenClipStatus(const float (&v)[4])
        {
            const float values[5] = {v[0], v[1], v[3], 4096.0f - v[0], 4096.0f - v[1]};
            int32_t status = 0;
            for (const float value : values)
            {
                const float c = vuClamp(value);
                if (c == 0.0f)
                    status |= 0x40;
                if (std::signbit(c))
                    status |= 0x80;
            }
            return status;
        }
    }

    void sceVpu0Reset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceVu0AddVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = lhs[i] + rhs[i];
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ApplyMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t matrixAddr = getRegU32(ctx, 5);
        const uint32_t srcAddr = getRegU32(ctx, 6);
        float matrix[16]{};
        float src[4]{};
        float out[4]{};
        if (readVuMatrix4f(rdram, matrixAddr, matrix) && readVuVec4f(rdram, srcAddr, src))
        {
            // Match libvux VuxApplyMatrix math while honoring the imported EE ABI:
            // a0=out, a1=matrix, a2=vector.
            out[0] = (matrix[0] * src[0]) + (matrix[4] * src[1]) + (matrix[8] * src[2]) + (matrix[12] * src[3]);
            out[1] = (matrix[1] * src[0]) + (matrix[5] * src[1]) + (matrix[9] * src[2]) + (matrix[13] * src[3]);
            out[2] = (matrix[2] * src[0]) + (matrix[6] * src[1]) + (matrix[10] * src[2]) + (matrix[14] * src[3]);
            out[3] = (matrix[3] * src[0]) + (matrix[7] * src[1]) + (matrix[11] * src[2]) + (matrix[15] * src[3]);
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0CameraMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t eyeAddr = getRegU32(ctx, 5);
        const uint32_t fwdAddr = getRegU32(ctx, 6);
        const uint32_t upAddr = getRegU32(ctx, 7);
        float eye[4]{}, fwd[4]{}, up[4]{};
        if (readVuVec4f(rdram, eyeAddr, eye) && readVuVec4f(rdram, fwdAddr, fwd) && readVuVec4f(rdram, upAddr, up))
        {
            auto cross = [](const float (&l)[4], const float (&r)[4], float (&o)[4])
            {
                o[0] = (l[1] * r[2]) - (l[2] * r[1]);
                o[1] = (l[2] * r[0]) - (l[0] * r[2]);
                o[2] = (l[0] * r[1]) - (l[1] * r[0]);
                o[3] = 0.0f;
            };
            auto normalize = [](const float (&s)[4], float (&o)[4])
            {
                normalizeXyz(s, o);
            };
            float rawCross[4]{}, row0[4]{}, row1[4]{}, row2[4]{};
            cross(up, fwd, rawCross);
            normalize(rawCross, row0);
            normalize(fwd, row2);
            cross(row2, row0, row1);

            float m[16]{};
            for (int i = 0; i < 4; ++i)
            {
                m[i] = row0[i];
                m[4 + i] = row1[i];
                m[8 + i] = row2[i];
            }
            m[12] = eye[0];
            m[13] = eye[1];
            m[14] = eye[2];
            m[15] = 1.0f;

            float out[16]{};
            rigidInverse(m, out);

            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ClampVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float lo = ctx ? ctx->f[12] : 0.0f;
        const float hi = ctx ? ctx->f[13] : 0.0f;
        float src[4]{}, out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                float v = src[i];
                v = (v < lo) ? lo : v;
                v = (v > hi) ? hi : v;
                out[i] = v;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ClipAll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // libvu0 (game copy at 0x10ff50): a0 = min, a1 = max, a2 = matrix, a3 = vertices, t0 = count.
        // For each vertex v' = v*m; inside when x - min.x*w, y - min.y*w, w - min.w and
        // max.x*w - x, max.y*w - y, max.w - w are all > 0 (no Z or S flag). Returns 0 at the first
        // inside vertex, 1 when every vertex is outside.
        const uint32_t loAddr = getRegU32(ctx, 4);
        const uint32_t hiAddr = getRegU32(ctx, 5);
        const uint32_t matAddr = getRegU32(ctx, 6);
        uint32_t vAddr = getRegU32(ctx, 7);
        const int32_t count = static_cast<int32_t>(getRegU32(ctx, 8));
        float lo[4]{}, hi[4]{}, m[16]{};
        int32_t result = 1;
        if (readVuVec4f(rdram, loAddr, lo) && readVuVec4f(rdram, hiAddr, hi) && readVuMatrix4f(rdram, matAddr, m))
        {
            // The SDK loop is a do-while on the count (0 would run 2^32 times); one vertex at least.
            int32_t remaining = count > 0 ? count : 1;
            do
            {
                float v[4]{};
                if (!readVuVec4f(rdram, vAddr, v))
                    break;
                const float tx = vuClamp((m[0] * v[0]) + (m[4] * v[1]) + (m[8] * v[2]) + (m[12] * v[3]));
                const float ty = vuClamp((m[1] * v[0]) + (m[5] * v[1]) + (m[9] * v[2]) + (m[13] * v[3]));
                const float tw = vuClamp((m[3] * v[0]) + (m[7] * v[1]) + (m[11] * v[2]) + (m[15] * v[3]));
                const float checks[6] = {tx - vuClamp(lo[0] * tw), ty - vuClamp(lo[1] * tw), tw - lo[3],
                                         vuClamp(hi[0] * tw) - tx, vuClamp(hi[1] * tw) - ty, hi[3] - tw};
                bool inside = true;
                for (const float c : checks)
                {
                    const float n = vuClamp(c);
                    if (n == 0.0f || std::signbit(n))
                        inside = false;
                }
                if (inside)
                {
                    result = 0;
                    break;
                }
                vAddr += 16u;
            } while (--remaining != 0);
        }
        setReturnS32(ctx, result);
    }

    void sceVu0ClipScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t vAddr = getRegU32(ctx, 4);
        float v[4]{};
        int32_t code = 0;
        if (readVuVec4f(rdram, vAddr, v))
        {
            code = screenClipStatus(v);
        }
        setReturnS32(ctx, code);
    }

    void sceVu0ClipScreen3(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t v0Addr = getRegU32(ctx, 4);
        const uint32_t v1Addr = getRegU32(ctx, 5);
        const uint32_t v2Addr = getRegU32(ctx, 6);
        float v0[4]{}, v1[4]{}, v2[4]{};
        int32_t code = 0;
        if (readVuVec4f(rdram, v0Addr, v0))
        {
            code |= screenClipStatus(v0);
        }
        if (readVuVec4f(rdram, v1Addr, v1))
        {
            code |= screenClipStatus(v1);
        }
        if (readVuVec4f(rdram, v2Addr, v2))
        {
            code |= screenClipStatus(v2);
        }
        setReturnS32(ctx, code);
    }

    void sceVu0CopyMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        uint8_t *dst = getMemPtr(rdram, dstAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (dst && src)
        {
            std::memcpy(dst, src, sizeof(float) * 16u);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0CopyVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        uint8_t *dst = getMemPtr(rdram, dstAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (dst && src)
        {
            std::memcpy(dst, src, sizeof(float) * 4u);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0CopyVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        uint8_t *dst = getMemPtr(rdram, dstAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (dst && src)
        {
            std::memcpy(dst, src, sizeof(float) * 3u);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0DivVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float divisor = ctx ? ctx->f[12] : 1.0f;
        float src[4]{}, out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            const float q = vuDivide(1.0f, divisor);
            for (int i = 0; i < 4; ++i)
            {
                out[i] = vuClamp(src[i] * q);
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0DivVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float divisor = ctx ? ctx->f[12] : 1.0f;
        float src[4]{}, out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            const float q = vuDivide(1.0f, divisor);
            out[0] = vuClamp(src[0] * q);
            out[1] = vuClamp(src[1] * q);
            out[2] = vuClamp(src[2] * q);
            out[3] = src[3];
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0DropShadowMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Two documented drop-shadow modes reconstructed from behavior; not
        // claimed bit-exact to a specific SDK build.
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t nAddr = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const float lx = ctx ? ctx->f[12] : 0.0f;
        const float ly = ctx ? ctx->f[13] : 0.0f;
        const float lz = ctx ? ctx->f[14] : 0.0f;
        float n[4]{};
        if (readVuVec4f(rdram, nAddr, n))
        {
            const float nx = n[0], ny = n[1], nz = n[2];
            const float d = (lx * nx) + (ly * ny) + (lz * nz);
            float out[16]{};
            if (mode != 0)
            {
                const float k = 1.0f - d;
                out[0] = (lx * nx) + k;
                out[1] = lx * ny;
                out[2] = lx * nz;
                out[3] = lx;
                out[4] = ly * nx;
                out[5] = (ly * ny) + k;
                out[6] = ly * nz;
                out[7] = ly;
                out[8] = lz * nx;
                out[9] = lz * ny;
                out[10] = (lz * nz) + k;
                out[11] = lz;
                out[12] = -nx;
                out[13] = -ny;
                out[14] = -nz;
                out[15] = -d;
            }
            else
            {
                const float k = (d != 0.0f) ? (-1.0f / d) : 0.0f;
                out[0] = k * ((lx * nx) - d);
                out[1] = k * (lx * ny);
                out[2] = k * (lx * nz);
                out[3] = 0.0f;
                out[4] = k * (ly * nx);
                out[5] = k * ((ly * ny) - d);
                out[6] = k * (ly * nz);
                out[7] = 0.0f;
                out[8] = k * (lz * nx);
                out[9] = k * (lz * ny);
                out[10] = k * ((lz * nz) - d);
                out[11] = 0.0f;
                out[12] = k * -nx;
                out[13] = k * -ny;
                out[14] = k * -nz;
                out[15] = 1.0f;
            }
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ecossin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const float angle = ctx ? ctx->f[12] : 0.0f;
        float out[4] = {std::cos(angle), std::sin(angle), 0.0f, 0.0f};
        (void)writeVuVec4f(rdram, dstAddr, out);
        setReturnS32(ctx, 0);
    }

    void sceVu0FTOI0Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{};
        int32_t out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<int32_t>(src[i]);
            }
            (void)writeVuVec4i(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0FTOI4Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{};
        int32_t out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<int32_t>(src[i] * 16.0f);
            }
            (void)writeVuVec4i(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0InnerProduct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t lhsAddr = getRegU32(ctx, 4);
        const uint32_t rhsAddr = getRegU32(ctx, 5);
        float lhs[4]{}, rhs[4]{};
        float dot = 0.0f;
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            // xyz only, like libvu0 (vmul.xyz, then x + y + z).
            dot = (lhs[0] * rhs[0]) + (lhs[1] * rhs[1]) + (lhs[2] * rhs[2]);
        }

        if (ctx)
        {
            ctx->f[0] = dot;
        }
        uint32_t raw = 0u;
        std::memcpy(&raw, &dot, sizeof(raw));
        setReturnU32(ctx, raw);
    }

    void sceVu0InterVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t aAddr = getRegU32(ctx, 5);
        const uint32_t bAddr = getRegU32(ctx, 6);
        const float t = ctx ? ctx->f[12] : 0.0f;
        float a[4]{}, b[4]{}, out[4]{};
        if (readVuVec4f(rdram, aAddr, a) && readVuVec4f(rdram, bAddr, b))
        {
            const float invT = 1.0f - t;
            for (int i = 0; i < 4; ++i)
            {
                out[i] = (a[i] * t) + (b[i] * invT);
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0InterVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t aAddr = getRegU32(ctx, 5);
        const uint32_t bAddr = getRegU32(ctx, 6);
        const float t = ctx ? ctx->f[12] : 0.0f;
        float a[4]{}, b[4]{}, out[4]{};
        if (readVuVec4f(rdram, aAddr, a) && readVuVec4f(rdram, bAddr, b))
        {
            const float invT = 1.0f - t;
            out[0] = (a[0] * t) + (b[0] * invT);
            out[1] = (a[1] * t) + (b[1] * invT);
            out[2] = (a[2] * t) + (b[2] * invT);
            out[3] = a[3];
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0InversMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float in[16]{}, out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, in))
        {
            rigidInverse(in, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ITOF0Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        int32_t src[4]{};
        float out[4]{};
        if (readVuVec4i(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<float>(src[i]);
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ITOF12Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        int32_t src[4]{};
        float out[4]{};
        if (readVuVec4i(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<float>(src[i]) / 4096.0f;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ITOF4Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        int32_t src[4]{};
        float out[4]{};
        if (readVuVec4i(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<float>(src[i]) / 16.0f;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0LightColorMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t c0Addr = getRegU32(ctx, 5);
        const uint32_t c1Addr = getRegU32(ctx, 6);
        const uint32_t c2Addr = getRegU32(ctx, 7);
        const uint32_t c3Addr = getRegU32(ctx, 8);
        float c0[4]{}, c1[4]{}, c2[4]{}, c3[4]{};
        if (readVuVec4f(rdram, c0Addr, c0) && readVuVec4f(rdram, c1Addr, c1) &&
            readVuVec4f(rdram, c2Addr, c2) && readVuVec4f(rdram, c3Addr, c3))
        {
            float out[16]{};
            for (int i = 0; i < 4; ++i)
            {
                out[i] = c0[i];
                out[4 + i] = c1[i];
                out[8 + i] = c2[i];
                out[12 + i] = c3[i];
            }
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0MulMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t m0Addr = getRegU32(ctx, 5);
        const uint32_t m1Addr = getRegU32(ctx, 6);
        float m0[16]{}, m1[16]{}, out[16]{};
        if (readVuMatrix4f(rdram, m0Addr, m0) && readVuMatrix4f(rdram, m1Addr, m1))
        {
            // sceVu0MulMatrix(m0, m1, m2): libvu0 loads m1's rows into vf4-vf7 and, for each row
            // of m2, writes row.x*m1[0] + row.y*m1[1] + row.z*m1[2] + row.w*m1[3]: m0 = m2 . m1
            // (row vectors; checked against the game's own copy at 0x10e9f8).
            mulVuMatrix(m1, m0, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0MulVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = lhs[i] * rhs[i];
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0Normalize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // libvu0: vsqrt of x*x+y*y+z*z, Q = 1/len, out.xyz = v.xyz*Q, out.w = 0.
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{}, out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            normalizeXyz(src, out);
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0NormalLightMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Rows = normalize(-light); the 4x4 is transposed so directions occupy
        // columns (one ApplyMatrix then yields per-light N.L).
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t l0Addr = getRegU32(ctx, 5);
        const uint32_t l1Addr = getRegU32(ctx, 6);
        const uint32_t l2Addr = getRegU32(ctx, 7);
        float l0[4]{}, l1[4]{}, l2[4]{};
        if (readVuVec4f(rdram, l0Addr, l0) && readVuVec4f(rdram, l1Addr, l1) && readVuVec4f(rdram, l2Addr, l2))
        {
            // libvu0: ScaleVector(tmp, light, -1) then Normalize(row, tmp) (xyz, w = 0).
            auto negNormalize = [](const float (&s)[4], float (&o)[4])
            {
                const float negated[4] = {-s[0], -s[1], -s[2], -s[3]};
                normalizeXyz(negated, o);
            };
            float r0[4]{}, r1[4]{}, r2[4]{};
            negNormalize(l0, r0);
            negNormalize(l1, r1);
            negNormalize(l2, r2);
            float m[16]{};
            for (int i = 0; i < 4; ++i)
            {
                m[i] = r0[i];
                m[4 + i] = r1[i];
                m[8 + i] = r2[i];
                m[12 + i] = 0.0f;
            }
            m[15] = 1.0f;
            float out[16]{};
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    out[4 * row + col] = m[4 * col + row];
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0OuterProduct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            out[0] = (lhs[1] * rhs[2]) - (lhs[2] * rhs[1]);
            out[1] = (lhs[2] * rhs[0]) - (lhs[0] * rhs[2]);
            out[2] = (lhs[0] * rhs[1]) - (lhs[1] * rhs[0]);
            out[3] = 0.0f;
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t rotAddr = getRegU32(ctx, 6);
        float src[16]{}, rotVec[4]{};
        if (readVuMatrix4f(rdram, srcAddr, src) && readVuVec4f(rdram, rotAddr, rotVec))
        {
            float afterZ[16]{}, afterY[16]{}, afterX[16]{};
            axisRotateMatrix(src, rotVec[2], 2, afterZ);
            axisRotateMatrix(afterZ, rotVec[1], 1, afterY);
            axisRotateMatrix(afterY, rotVec[0], 0, afterX);
            (void)writeVuMatrix4f(rdram, dstAddr, afterX);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrixX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float angle = ctx ? ctx->f[12] : 0.0f;
        float src[16]{}, rot[16]{}, out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, src))
        {
            makeIdentityMatrix(rot);
            const float cs = std::cos(angle);
            const float sn = std::sin(angle);
            rot[5] = cs;
            rot[6] = sn;
            rot[9] = -sn;
            rot[10] = cs;
            mulVuMatrix(src, rot, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrixY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float angle = ctx ? ctx->f[12] : 0.0f;
        float src[16]{}, rot[16]{}, out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, src))
        {
            makeIdentityMatrix(rot);
            const float cs = std::cos(angle);
            const float sn = std::sin(angle);
            rot[0] = cs;
            rot[2] = -sn;
            rot[8] = sn;
            rot[10] = cs;
            mulVuMatrix(src, rot, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrixZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float angle = ctx ? ctx->f[12] : 0.0f;
        float src[16]{}, rot[16]{}, out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, src))
        {
            makeIdentityMatrix(rot);
            const float cs = std::cos(angle);
            const float sn = std::sin(angle);
            rot[0] = cs;
            rot[1] = sn;
            rot[4] = -sn;
            rot[5] = cs;
            mulVuMatrix(src, rot, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0RotTransPers(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t matAddr = getRegU32(ctx, 5);
        const uint32_t vAddr = getRegU32(ctx, 6);
        const bool zwFtoi0 = (getRegU32(ctx, 7) != 0);
        float m[16]{}, v[4]{};
        if (readVuMatrix4f(rdram, matAddr, m) && readVuVec4f(rdram, vAddr, v))
        {
            int32_t out[4]{};
            rotTransPersOne(m, v, zwFtoi0, out);
            (void)writeVuVec4i(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0RotTransPersN(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t matAddr = getRegU32(ctx, 5);
        uint32_t vAddr = getRegU32(ctx, 6);
        const int32_t count = static_cast<int32_t>(getRegU32(ctx, 7));
        const bool zwFtoi0 = (getRegU32(ctx, 8) != 0);
        float m[16]{};
        if (readVuMatrix4f(rdram, matAddr, m))
        {
            uint32_t outAddr = dstAddr;
            for (int32_t i = 0; i < count; ++i)
            {
                float v[4]{};
                if (!readVuVec4f(rdram, vAddr, v))
                    break;
                int32_t out[4]{};
                rotTransPersOne(m, v, zwFtoi0, out);
                (void)writeVuVec4i(rdram, outAddr, out);
                vAddr += 16u;
                outAddr += 16u;
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ScaleVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{}, out[4]{};
        // libvu0: the scale is the float argument in f12 (vmulx.xyzw by it); a zero scale is 0.
        const float scale = ctx ? ctx->f[12] : 0.0f;

        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = src[i] * scale;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ScaleVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float scale = ctx ? ctx->f[12] : 0.0f;
        float src[4]{}, out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            out[0] = src[0] * scale;
            out[1] = src[1] * scale;
            out[2] = src[2] * scale;
            out[3] = src[3];
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0SubVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = lhs[i] - rhs[i];
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0TransMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t vAddr = getRegU32(ctx, 6);
        float src[16]{}, v[4]{};
        if (readVuMatrix4f(rdram, srcAddr, src) && readVuVec4f(rdram, vAddr, v))
        {
            float out[16]{};
            std::memcpy(out, src, sizeof(out));
            out[12] = src[12] + v[0];
            out[13] = src[13] + v[1];
            out[14] = src[14] + v[2];
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0TransposeMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[16]{};
        float out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, src))
        {
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    out[4 * row + col] = src[4 * col + row];
                }
            }
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0UnitMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4); // sceVu0FMATRIX dst
        alignas(16) const float identity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};

        if (!writeGuestBytes(rdram, runtime, dstAddr, reinterpret_cast<const uint8_t *>(identity), sizeof(identity)))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 8)
            {
                std::cerr << "sceVu0UnitMatrix: failed to write matrix at 0x"
                          << std::hex << dstAddr << std::dec << std::endl;
                ++warnCount;
            }
        }

        setReturnS32(ctx, 0);
    }

    void sceVu0ViewScreenMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Near/far params handled by formula shape; SDK parameter names
        // not pinned. Args follow the out-of-line libvu0 register convention
        // used throughout this file: eight scalar floats in f12..f19 (p0..p7)
        // and the ninth (p8) as the first stack-passed argument. That
        // eight-FP-arg-register layout is the n32/EABI convention the EE
        // toolchain emits (an o32 layout would carry only two FP args in
        // f12/f14 and spill p2..p7 too), and under it no GPR home/save area is
        // reserved, so the ninth float is at 0(sp) -- read below. A target ABI
        // that reserves a home area would shift only that read by its size.
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const float p0 = ctx ? ctx->f[12] : 0.0f;
        const float p1 = ctx ? ctx->f[13] : 0.0f;
        const float p2 = ctx ? ctx->f[14] : 0.0f;
        const float p3 = ctx ? ctx->f[15] : 0.0f;
        const float p4 = ctx ? ctx->f[16] : 0.0f;
        const float p5 = ctx ? ctx->f[17] : 0.0f;
        const float p6 = ctx ? ctx->f[18] : 0.0f;
        const float p7 = ctx ? ctx->f[19] : 0.0f;
        float p8 = 0.0f;
        if (const uint8_t *sp = getConstMemPtr(rdram, getRegU32(ctx, 29)))
        {
            std::memcpy(&p8, sp, sizeof(p8));
        }

        const float denom = p8 - p7;
        const float zScale = (denom != 0.0f) ? ((p8 * p7 * (p6 - p5)) / denom) : 0.0f;
        const float zOffset = (denom != 0.0f) ? (((p5 * p8) - (p6 * p7)) / denom) : 0.0f;

        float scaleMat[16]{};
        makeIdentityMatrix(scaleMat);
        scaleMat[0] = p0;
        scaleMat[5] = p0;
        scaleMat[10] = 0.0f;
        scaleMat[11] = 1.0f;
        scaleMat[14] = 1.0f;
        scaleMat[15] = 0.0f;

        float projMat[16]{};
        makeIdentityMatrix(projMat);
        projMat[0] = p1;
        projMat[5] = p2;
        projMat[10] = zScale;
        projMat[12] = p3;
        projMat[13] = p4;
        projMat[14] = zOffset;

        float out[16]{};
        mulVuMatrix(scaleMat, projMat, out);
        (void)writeVuMatrix4f(rdram, dstAddr, out);
        setReturnS32(ctx, 0);
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    // No state of its own: only this file's copy of the Helpers/Support.h globals.
    void saveVu(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
    }

    void loadVu(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
    }
}
