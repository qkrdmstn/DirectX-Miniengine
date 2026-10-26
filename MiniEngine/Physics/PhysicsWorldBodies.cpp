#include "PhysicsWorldInternal.h"
#include "Collision/AggregateGeom.h"
#include "Collision/BodyMass.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <limits>

namespace MiniPhysics
{
    namespace
    {
        // 실패 사유를 호출자 문자열에 남기고 false를 반환한다.
        bool Fail(std::string* error, const std::string& message)
        {
            if (error) *error = message;
            return false;
        }

        // 배열의 모든 성분이 유한한지 검사한다.
        template <size_t N>
        bool AllFinite(const std::array<double, N>& values)
        {
            for (const double value : values)
                if (!std::isfinite(value)) return false;
            return true;
        }

        // 쿼터니언을 최대 성분으로 나눈 뒤 double에서 정규화해 float 쿼터니언으로 바꾼다. 제곱 오버플로가 없고 {1e30,0,0,1e30}도 받아들인다.
        // 최대 성분이 1e-30 미만이면 영벡터로 보고 거절하며, 변환 결과가 float에서 유한한 단위 쿼터니언인지 다시 확인한다.
        bool NormalizeRotation(const std::array<double, 4>& q, JPH::Quat& out)
        {
            // 최대 성분으로 스케일해 오버플로를 막는다.
            const double peak = std::max({ std::abs(q[0]), std::abs(q[1]), std::abs(q[2]), std::abs(q[3]) });
            if (!(peak >= 1.0e-30)) return false;
            const double x = q[0] / peak, y = q[1] / peak, z = q[2] / peak, w = q[3] / peak;
            const double length = std::sqrt(x * x + y * y + z * z + w * w);

            // float 변환 뒤에도 유한하고 단위 길이에 가까운지 확인한다.
            out = JPH::Quat(float(x / length), float(y / length), float(z / length), float(w / length));
            const float check = out.LengthSq();
            return std::isfinite(check) && std::abs(check - 1.0f) < 1.0e-3f;
        }

        // 요청 값의 유한성·범위·scale 양수를 검사한다. 음수 scale은 삼각형 감김을 뒤집으므로 받지 않는다.
        bool ValidateDesc(const BodyDesc& desc, std::string* error)
        {
            // 지오메트리 존재와 요소 수를 확인한다.
            if (!desc.Geometry || desc.Geometry->ConvexElems.empty()) return Fail(error, "Empty geometry");

            // pose와 scale이 유한하고 scale이 양수인지 확인한다.
            if (!AllFinite(desc.Scale) || !AllFinite(desc.Position) || !AllFinite(desc.Rotation)) return Fail(error, "Non-finite pose or scale");
            for (const double s : desc.Scale)
                if (s <= 0.0) return Fail(error, "Scale must be positive");
            for (const double p : desc.Position)
                if (std::abs(p) > 1.0e9) return Fail(error, "Position out of range");

            // 질량 배율과 재질 값을 확인한다.
            if (!std::isfinite(desc.MassScale) || desc.MassScale <= 0.0) return Fail(error, "Invalid mass scale");
            if (!std::isfinite(desc.Friction) || desc.Friction < 0.0f || !std::isfinite(desc.Restitution) || desc.Restitution < 0.0f) return Fail(error, "Invalid material");
            return true;
        }

        // 요소마다 scale을 곱한 ConvexHullShape을 cook하고 여러 개면 StaticCompoundShape으로 묶는다. 스케일된 부피 합도 구한다.
        // convex radius와 hull tolerance는 Jolt 기본값(0.05 m, 1e-3)을 그대로 쓴다. 반지름은 작은 hull에서 Jolt가 스스로 줄인다.
        bool CookShape(const BodyDesc& desc, JPH::RefConst<JPH::Shape>& shape, double& volume, std::string* error)
        {
            // 요소별 부피 배율과 결과 누적 변수를 준비한다.
            const auto& elems = desc.Geometry->ConvexElems;
            const double volumeScale = desc.Scale[0] * desc.Scale[1] * desc.Scale[2];
            JPH::StaticCompoundShapeSettings compound;
            volume = 0.0;

            // 요소마다 scale이 곱해진 점으로 hull을 cook한다.
            for (const auto& elem : elems)
            {
                if (elem.VertexData.size() < 4) return Fail(error, "Convex element needs 4+ vertices");
                JPH::Array<JPH::Vec3> points;
                points.reserve(elem.VertexData.size());
                for (const auto& v : elem.VertexData)
                {
                    const float x = float(v.x * desc.Scale[0]), y = float(v.y * desc.Scale[1]), z = float(v.z * desc.Scale[2]);
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return Fail(error, "Non-finite vertex");
                    points.push_back(JPH::Vec3(x, y, z));
                }
                const JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(points).Create();
                if (result.HasError()) return Fail(error, std::string("Hull cook failed: ") + result.GetError().c_str());

                // 부피를 누적하고 단일 요소는 그대로, 여러 요소는 compound에 원점 기준으로 추가한다.
                volume += elem.GetVolume() * volumeScale;
                if (elems.size() == 1) { shape = result.Get(); return true; }
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), result.Get());
            }

            // 여러 요소를 하나의 compound shape으로 만든다.
            const JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.HasError()) return Fail(error, std::string("Compound cook failed: ") + result.GetError().c_str());
            shape = result.Get();
            return true;
        }

        // 미게시 body를 파괴하고 예약을 반환한다. 고아(Owner 없음)면 world가 이미 body를 정리했으므로 아무것도 하지 않는다.
        void ReleasePrepared(PreparedBody::Data& data)
        {
            if (!data.Owner) return;
            data.Owner->World.GetBodyInterface().DestroyBody(data.Id);
            data.Owner->Prepared.erase(&data);
        }
    }

    // 빈 객체를 만든다.
    PreparedBody::PreparedBody() = default;

    // 미게시 body가 남아 있으면 파괴한다.
    PreparedBody::~PreparedBody()
    {
        if (m_Data) ReleasePrepared(*m_Data);
    }

    // 소유권을 넘겨받는다. 원본은 빈 상태가 된다.
    PreparedBody::PreparedBody(PreparedBody&& other) noexcept : m_Data(std::move(other.m_Data)) {}

    // 현재 body를 정리한 뒤 소유권을 넘겨받는다.
    PreparedBody& PreparedBody::operator=(PreparedBody&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Data) ReleasePrepared(*m_Data);
            m_Data = std::move(other.m_Data);
        }
        return *this;
    }

    // 지오메트리를 cook하고 질량을 계산해 world에 올리지 않은 body를 만든다.
    bool PhysicsWorld::PrepareBody(const BodyDesc& desc, PreparedBody& out, std::string* error)
    {
        // 이전 준비 body를 비우고 초기화·용량·handle 여유를 확인한다.
        out = PreparedBody();
        if (!m_Impl) return Fail(error, "World not initialized");
        if (m_Impl->Reserved() >= m_Impl->Capacity) return Fail(error, "Body capacity exhausted");
        if (m_NextBodyHandle == std::numeric_limits<BodyHandle>::max()) return Fail(error, "Body handle exhausted");
        if (!ValidateDesc(desc, error)) return false;

        try
        {
            // 형상을 cook하고 동적 body의 부피를 확인한다.
            JPH::RefConst<JPH::Shape> shape;
            double volume = 0.0;
            if (!CookShape(desc, shape, volume, error)) return false;
            const bool dynamic = desc.Motion == BodyMotion::Dynamic;
            if (dynamic && !(std::isfinite(volume) && volume > 0.0)) return Fail(error, "Non-positive volume");

            // 동적 body의 최종 질량이 float로 표현 가능한 양수인지 body 생성 전에 확인한다.
            double mass = 0.0;
            if (dynamic)
            {
                mass = MiniCollision::CalculateUnrealMass(volume, desc.MassScale);
                if (!(std::isfinite(mass) && mass > 0.0 && mass <= double(FLT_MAX))) return Fail(error, "Mass out of float range");
            }

            // body 원점 pose와 재질로 생성 설정을 만든다.
            JPH::Quat rotation;
            if (!NormalizeRotation(desc.Rotation, rotation)) return Fail(error, "Invalid rotation");
            const JPH::RVec3 position(float(desc.Position[0]), float(desc.Position[1]), float(desc.Position[2]));
            JPH::BodyCreationSettings settings(shape, position, rotation,
                dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                dynamic ? ObjectLayers::Moving : ObjectLayers::Static);
            settings.mFriction = desc.Friction;
            settings.mRestitution = desc.Restitution;

            // 동적 body는 Unreal 질량을 쓰고 관성은 shape에서 계산해 질량에 맞춘다.
            if (dynamic)
            {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = float(mass);
            }

            // world에 추가하지 않고 body만 만든다.
            auto& bodies = m_Impl->World.GetBodyInterface();
            JPH::Body* body = bodies.CreateBody(settings);
            if (!body) return Fail(error, "Jolt body creation failed");
            const JPH::BodyID id = body->GetID();

            // handle 맵 노드를 미리 할당하고 추적 목록에 올린다. 실패하면 body를 되돌린다.
            try
            {
                auto data = std::make_unique<PreparedBody::Data>();
                SimBodyMap holder;
                holder.emplace(InvalidBody, id);
                data->Node = holder.extract(InvalidBody);
                data->Owner = m_Impl.get();
                data->Id = id;
                m_Impl->Prepared.insert(data.get());
                out.m_Data = std::move(data);
            }
            catch (...)
            {
                bodies.DestroyBody(id);
                throw;
            }
            return true;
        }
        catch (const std::exception& e)
        {
            return Fail(error, std::string("Exception: ") + e.what());
        }
    }

    // 준비된 body를 world에 올리고 handle을 발급한다. 맵 노드는 이미 할당되어 있어 실패하지 않는다.
    BodyHandle PhysicsWorld::CommitBody(PreparedBody&& body, bool activate)
    {
        // 같은 world에서 준비된 body만 받는다.
        if (!m_Impl || !body.m_Data || body.m_Data->Owner != m_Impl.get()) return InvalidBody;
        if (m_NextBodyHandle == std::numeric_limits<BodyHandle>::max()) return InvalidBody;

        // 준비 목록에서 빼 handle을 부여하고 등록한다.
        auto data = std::move(body.m_Data);
        const BodyHandle handle = m_NextBodyHandle++;
        data->Node.key() = handle;
        m_Impl->Prepared.erase(data.get());
        m_Impl->SimBodies.insert(std::move(data->Node));

        // body를 broadphase에 올린다.
        m_Impl->World.GetBodyInterface().AddBody(data->Id, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        return handle;
    }

    // body를 broadphase에서만 제거하고 살려 둔다.
    bool PhysicsWorld::RemoveBodyFromWorld(BodyHandle handle)
    {
        // handle을 찾는다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->SimBodies.find(handle);
        if (entry == m_Impl->SimBodies.end()) return false;

        // 올라가 있는 body만 broadphase에서 뺀다.
        auto& bodies = m_Impl->World.GetBodyInterface();
        if (!bodies.IsAdded(entry->second)) return false;
        bodies.RemoveBody(entry->second);
        return true;
    }

    // 제거해 둔 body를 다시 broadphase에 올린다.
    bool PhysicsWorld::AddBodyToWorld(BodyHandle handle, bool activate)
    {
        // handle을 찾는다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->SimBodies.find(handle);
        if (entry == m_Impl->SimBodies.end()) return false;

        // 빠져 있는 body만 다시 올린다.
        auto& bodies = m_Impl->World.GetBodyInterface();
        if (bodies.IsAdded(entry->second)) return false;
        bodies.AddBody(entry->second, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        return true;
    }

    // body를 필요하면 제거한 뒤 파괴하고 handle을 등록 해제한다.
    bool PhysicsWorld::DestroyBody(BodyHandle handle)
    {
        // handle을 찾는다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->SimBodies.find(handle);
        if (entry == m_Impl->SimBodies.end()) return false;

        // 올라가 있으면 먼저 뺀다.
        auto& bodies = m_Impl->World.GetBodyInterface();
        if (bodies.IsAdded(entry->second)) bodies.RemoveBody(entry->second);

        // body를 파괴하고 handle을 등록 해제한다.
        bodies.DestroyBody(entry->second);
        m_Impl->SimBodies.erase(entry);
        return true;
    }

    // 게시된 simulation body 수를 반환한다.
    size_t PhysicsWorld::GetBodyCount() const { return m_Impl ? m_Impl->SimBodies.size() : 0; }

    // 조회·simulation·준비 중 body의 예약 합계를 반환한다.
    size_t PhysicsWorld::GetReservedCount() const { return m_Impl ? m_Impl->Reserved() : 0; }
}
