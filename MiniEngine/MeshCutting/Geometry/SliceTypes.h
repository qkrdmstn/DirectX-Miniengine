#pragma once

// 이 파일은 절단 코드 여러 곳이 함께 쓰는 작은 타입(SliceStatus, kInvalidIndex)과 보조 함수
// (오류 문자열 Fail, 유한값 검사 Finite, 숫자 문자열 Num, double 3D 벡터와 Clamp)를 모은다.
// Model과 Core에 의존하지 않는다. 설계: Docs/ClassLayoutDesign.md 6절, 7.1절.

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>
#include <string>

namespace MeshCutting
{
    constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    enum class SliceStatus
    {
        Ok,                     // both pieces are non-empty; loops may be empty
        NoCut,                  // every face lies on one side (touching included)
        InvalidInput,           // non-finite value, bad plane, index out of range
        InvalidOrigin,          // bary out of range or sum != 1, more than three origin vertices
        NonManifoldInput,       // edge used by 3+ faces, or two faces use it the same way
        DegenerateInput,        // zero-area face or repeated vertex in a face
        CoplanarFace,           // a face lies in the plane
        CrossesCap,             // the plane cuts an existing cap face
        OpenLoop,               // cut loop runs into a Replacement/UnweldedSeam boundary (OpenSurface ends are allowed)
        NonManifoldLoop,        // cut edges do not form simple loops
        InvalidLoop,            // self/mutual crossing, unsupported nesting, wrong winding
        CapTriangulationFailed,
        LoopOutsidePlane        // set by the caller (CutController), never by SliceGeometry: a cut loop leaves the finite CutPlane
    };

    // detail이 있으면 message를 적고 status를 그대로 돌려준다.
    inline SliceStatus Fail(std::string* detail, SliceStatus status, const std::string& message)
    {
        if (detail)
            *detail = message;
        return status;
    }

    inline bool Finite(const DirectX::XMFLOAT2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }
    inline bool Finite(const DirectX::XMFLOAT3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
    inline bool Finite(const DirectX::XMFLOAT4& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

    inline std::string Num(uint64_t value) { return std::to_string(value); }

    template <class T>
    T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // 광선, 피킹, 드래그처럼 오차가 쌓이기 쉬운 계산을 double로 하기 위한 3D 벡터.
    struct V3
    {
        double x = 0.0, y = 0.0, z = 0.0;
    };

    inline V3 ToV3(const DirectX::XMFLOAT3& v) { return { v.x, v.y, v.z }; }
    inline DirectX::XMFLOAT3 ToFloat3(const V3& v) { return { float(v.x), float(v.y), float(v.z) }; }
    inline V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline V3 operator*(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
    inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline double Length(const V3& a) { return std::sqrt(Dot(a, a)); }
    inline V3 Normalize(const V3& a)
    {
        const double length = Length(a);
        return length > 0.0 ? a * (1.0 / length) : a;
    }
}
