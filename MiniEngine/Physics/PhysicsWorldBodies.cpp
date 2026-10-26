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
            const double x = q[0] / peak;
            const double y = q[1] / peak;
            const double z = q[2] / peak;
            const double w = q[3] / peak;
            const double length = std::sqrt(x * x + y * y + z * z + w * w);

            // float 변환 뒤에도 유한하고 단위 길이에 가까운지 확인한다.
            out = JPH::Quat(float(x / length), float(y / length), float(z / length), float(w / length));
            const float check = out.LengthSq();
            return std::isfinite(check) && std::abs(check - 1.0f) < 1.0e-3f;
        }

        // 지오메트리·scale·질량 배율을 검사한다. 음수 scale은 삼각형 감김을 뒤집으므로 받지 않는다.
        bool ValidateShapeInput(const MiniCollision::AggregateGeom* geometry, const std::array<double, 3>& scale, double massScale, std::string* error)
        {
            // 지오메트리 존재와 요소 수를 확인한다.
            if (!geometry || geometry->ConvexElems.empty()) return Fail(error, "Empty geometry");

            // scale이 유한하고 양수인지 확인한다.
            if (!AllFinite(scale)) return Fail(error, "Non-finite pose or scale");
            for (const double s : scale)
                if (s <= 0.0) return Fail(error, "Scale must be positive");

            // 질량 배율을 확인한다.
            if (!std::isfinite(massScale) || massScale <= 0.0) return Fail(error, "Invalid mass scale");
            return true;
        }

        // 요청 값의 유한성·범위를 검사한다. shape 관련 검사는 ValidateShapeInput을 쓴다.
        bool ValidateDesc(const BodyDesc& desc, std::string* error)
        {
            // Static Box와 convex 요청을 구분하고 모호한 형상 지정을 거절한다.
            if (!desc.Geometry)
            {
                if (desc.Motion != BodyMotion::Static || !AllFinite(desc.StaticBoxHalfExtent) ||
                    desc.Scale != std::array<double, 3>{ 1, 1, 1 }) return Fail(error, "Invalid static box request");
                for (const double extent : desc.StaticBoxHalfExtent)
                    if (extent < 0.0001 || extent > 1e8) return Fail(error, "Static box extent out of range");
                if (!std::isfinite(desc.MassScale) || desc.MassScale <= 0.0) return Fail(error, "Invalid mass scale");
            }
            else
            {
                if (desc.StaticBoxHalfExtent != std::array<double, 3>{}) return Fail(error, "Ambiguous body shape");
                if (!ValidateShapeInput(desc.Geometry, desc.Scale, desc.MassScale, error)) return false;
            }

            // pose가 유한하고 범위 안인지 확인한다.
            if (!AllFinite(desc.Position) || !AllFinite(desc.Rotation)) return Fail(error, "Non-finite pose or scale");
            for (const double p : desc.Position)
                if (std::abs(p) > 1.0e9) return Fail(error, "Position out of range");

            // 재질 값을 확인한다.
            if (!std::isfinite(desc.Friction) || desc.Friction < 0.0f || !std::isfinite(desc.Restitution) || desc.Restitution < 0.0f) return Fail(error, "Invalid material");
            return true;
        }

        // 요소마다 scale을 곱한 ConvexHullShape을 cook하고 여러 개면 StaticCompoundShape으로 묶는다. 스케일된 부피 합도 구한다.
        // convex radius와 hull tolerance는 Jolt 기본값(0.05 m, 1e-3)을 그대로 쓴다. 반지름은 작은 hull에서 Jolt가 스스로 줄인다.
        bool CookShape(const MiniCollision::AggregateGeom& geometry, const std::array<double, 3>& scale, JPH::RefConst<JPH::Shape>& shape, double& volume, std::string* error)
        {
            // 요소별 부피 배율과 결과 누적 변수를 준비한다.
            const auto& elems = geometry.ConvexElems;
            const double volumeScale = scale[0] * scale[1] * scale[2];
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
                    const float x = float(v.x * scale[0]);
                    const float y = float(v.y * scale[1]);
                    const float z = float(v.z * scale[2]);
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return Fail(error, "Non-finite vertex");
                    points.push_back(JPH::Vec3(x, y, z));
                }
                const JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(points).Create();
                if (result.HasError()) return Fail(error, std::string("Hull cook failed: ") + result.GetError().c_str());

                // 부피를 누적하고 단일 요소는 그대로, 여러 요소는 compound에 원점 기준으로 추가한다.
                volume += elem.GetVolume() * volumeScale;
                if (elems.size() == 1)
                {
                    shape = result.Get();
                    return true;
                }
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), result.Get());
            }

            // 여러 요소를 하나의 compound shape으로 만든다.
            const JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.HasError()) return Fail(error, std::string("Compound cook failed: ") + result.GetError().c_str());
            shape = result.Get();
            return true;
        }

        // 검증된 지오메트리로 shape를 cook하고 동적 body의 Unreal 질량을 계산한다. PrepareBody와 SetBodyShape가 공유한다.
        // 정적 body의 질량은 0으로 두며, 어느 단계든 실패하면 body에 손대지 않고 false를 반환한다.
        bool BuildShapeAndMass(const MiniCollision::AggregateGeom& geometry, const std::array<double, 3>& scale, double massScale, bool dynamic,
            JPH::RefConst<JPH::Shape>& shape, double& mass, std::string* error)
        {
            // 형상을 cook하고 동적 body의 부피를 확인한다.
            double volume = 0.0;
            mass = 0.0;
            if (!CookShape(geometry, scale, shape, volume, error)) return false;
            if (dynamic && !(std::isfinite(volume) && volume > 0.0)) return Fail(error, "Non-positive volume");

            // 동적 body의 최종 질량이 float로 표현 가능한 양수인지 확인한다.
            if (dynamic)
            {
                mass = MiniCollision::CalculateUnrealMass(volume, massScale);
                if (!(std::isfinite(mass) && mass > 0.0 && mass <= double(FLT_MAX))) return Fail(error, "Mass out of float range");
            }
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
            // 바닥용 Static Box는 명시적 반크기를 쓰고 convex는 공유 helper로 질량까지 만든다.
            JPH::RefConst<JPH::Shape> shape;
            double mass = 0.0;
            const bool dynamic = desc.Motion == BodyMotion::Dynamic;
            if (!desc.Geometry)
            {
                const auto& half = desc.StaticBoxHalfExtent;
                const auto result = JPH::BoxShapeSettings(JPH::Vec3(float(half[0]), float(half[1]), float(half[2])), 0.0f).Create();
                if (result.HasError()) return Fail(error, std::string("Box cook failed: ") + result.GetError().c_str());
                shape = result.Get();
            }
            else if (!BuildShapeAndMass(*desc.Geometry, desc.Scale, desc.MassScale, dynamic, shape, mass, error)) return false;

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

    // body 원점을 순간이동한다(Unreal SetBodyTransform). 속도는 건드리지 않고, 월드에 올라간 동적 body는 깨운다.
    bool PhysicsWorld::SetBodyTransform(BodyHandle handle, const std::array<double, 3>& position, const std::array<double, 4>& rotation)
    {
        // handle을 찾는다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->SimBodies.find(handle);
        if (entry == m_Impl->SimBodies.end()) return false;

        // 입력이 유한하고 범위 안이며 회전이 정규화되는지 확인한다.
        JPH::Quat q;
        if (!AllFinite(position) || !AllFinite(rotation)) return false;
        for (const double p : position)
            if (std::abs(p) > 1.0e9) return false;
        if (!NormalizeRotation(rotation, q)) return false;

        // 월드에 올라간 body만 깨우고 빠져 있으면 pose만 바꾼다. 정적 body는 Jolt가 활성화하지 않는다.
        auto& bodies = m_Impl->World.GetBodyInterface();
        const JPH::EActivation activation = bodies.IsAdded(entry->second) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
        bodies.SetPositionAndRotation(entry->second, JPH::RVec3(float(position[0]), float(position[1]), float(position[2])), q, activation);
        return true;
    }

    // shape와 질량을 통째로 교체한다. 새 shape·질량을 먼저 완성하고, 하나라도 실패하면 body를 전혀 바꾸지 않는다.
    bool PhysicsWorld::SetBodyShape(BodyHandle handle, const MiniCollision::AggregateGeom& geometry, const std::array<double, 3>& scale, double massScale, std::string* error)
    {
        // handle을 찾고 입력을 검증한다.
        if (!m_Impl) return Fail(error, "World not initialized");
        const auto entry = m_Impl->SimBodies.find(handle);
        if (entry == m_Impl->SimBodies.end()) return Fail(error, "Unknown body");
        if (!ValidateShapeInput(&geometry, scale, massScale, error)) return false;

        try
        {
            // body의 운동 방식을 읽는다.
            const JPH::BodyID id = entry->second;
            bool dynamic = false;
            {
                const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), id);
                if (!lock.Succeeded()) return Fail(error, "Unknown body");
                dynamic = lock.GetBody().IsDynamic();
            }

            // body를 건드리기 전에 새 shape와 질량 속성을 모두 만든다.
            JPH::RefConst<JPH::Shape> shape;
            double mass = 0.0;
            if (!BuildShapeAndMass(geometry, scale, massScale, dynamic, shape, mass, error)) return false;
            JPH::MassProperties massProperties;
            if (dynamic)
            {
                massProperties = shape->GetMassProperties();
                massProperties.ScaleToMass(float(mass));
            }

            // shape만 교체한다. 원점 pose와 속도는 유지되고 질량은 아래에서 직접 설정한다.
            m_Impl->World.GetBodyInterface().SetShape(id, shape, false, JPH::EActivation::DontActivate);

            // 동적 body는 쓰기 잠금 아래에서 새 질량과 관성을 설정한다.
            if (dynamic)
            {
                const JPH::BodyLockWrite lock(m_Impl->World.GetBodyLockInterface(), id);
                if (lock.Succeeded())
                    lock.GetBody().GetMotionProperties()->SetMassProperties(JPH::EAllowedDOFs::All, massProperties);
            }

            // shape와 질량이 모두 바뀐 뒤 월드에 올라간 동적 body를 깨워 새 접촉을 풀게 한다.
            auto& bodies = m_Impl->World.GetBodyInterface();
            if (dynamic && bodies.IsAdded(id))
                bodies.ActivateBody(id);
            return true;
        }
        catch (const std::exception& e)
        {
            return Fail(error, std::string("Exception: ") + e.what());
        }
    }

    // 게시된 simulation body 수를 반환한다.
    size_t PhysicsWorld::GetBodyCount() const { return m_Impl ? m_Impl->SimBodies.size() : 0; }

    // 조회·simulation·준비 중 body의 예약 합계를 반환한다.
    size_t PhysicsWorld::GetReservedCount() const { return m_Impl ? m_Impl->Reserved() : 0; }
}
