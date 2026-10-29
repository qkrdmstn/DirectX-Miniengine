#pragma once

#include "SceneTransform.h"
#include "EditTarget.h"
#include <cstdint>
#include <cmath>
#include <vector>

namespace Viewer
{
    enum class HistoryTransformMode { Owner, Attached, WorldIndependent };

    // One completed drag is one command.  IDs survive deletion without retaining objects.
    class TransformHistory
    {
    public:
        struct Command
        {
            EditTarget Target;
            HistoryTransformMode Mode;
            SceneTransform Before;
            SceneTransform After;
        };

        // 대상과 변환 모드 및 변경 전후의 월드 TRS를 기록한다.
        bool Record(const EditTarget& target, HistoryTransformMode mode,
            const SceneTransform& before, const SceneTransform& after)
        {
            // 기존 허용오차로 무변화와 빈 대상을 제외한다.
            using namespace DirectX;
            const XMVECTOR tolerance = XMVectorReplicate(0.000001f);
            const bool sameRotation = XMVector4NearEqual(before.GetRotation(), after.GetRotation(), tolerance) ||
                XMVector4NearEqual(before.GetRotation(), -after.GetRotation(), tolerance);
            if (target.IsNone() || (XMVector3NearEqual(before.GetTranslation(), after.GetTranslation(), tolerance) &&
                sameRotation && XMVector3NearEqual(before.GetScale(), after.GetScale(), tolerance * Math::Max(before.GetScale(), after.GetScale()))))
                return false;

            // 최근 128개의 완료 명령만 보관한다.
            if (m_Commands.size() == 128)
                m_Commands.erase(m_Commands.begin());
            m_Commands.push_back({ target, mode, before, after });
            return true;
        }

        // 삭제되거나 적용이 실패한 명령을 소비하고 직전 유효 명령을 복구한다.
        template<class Apply>
        bool Undo(Apply&& apply)
        {
            // 적용에 성공할 때까지 최근 명령부터 전달한다.
            while (!m_Commands.empty())
            {
                const Command command = m_Commands.back();
                m_Commands.pop_back();
                if (apply(command))
                    return true;
            }
            return false;
        }

    private:
        std::vector<Command> m_Commands;
    };
}
