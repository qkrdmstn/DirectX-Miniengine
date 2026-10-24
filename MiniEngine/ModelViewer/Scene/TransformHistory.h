#pragma once

#include "SceneTransform.h"
#include <cstdint>
#include <cmath>
#include <vector>

namespace Viewer
{
    // One completed drag is one command.  IDs survive deletion without retaining objects.
    class TransformHistory
    {
    public:
        // 축별 TRS의 변경 한 번을 기록한다.
        bool Record(uint64_t id, const SceneTransform& before, const SceneTransform& after)
        {
            using namespace DirectX;
            const XMVECTOR tolerance = XMVectorReplicate(0.000001f);
            const bool sameRotation = XMVector4NearEqual(before.GetRotation(), after.GetRotation(), tolerance) ||
                XMVector4NearEqual(before.GetRotation(), -after.GetRotation(), tolerance);
            if (id == 0 || (XMVector3NearEqual(before.GetTranslation(), after.GetTranslation(), tolerance) &&
                sameRotation && XMVector3NearEqual(before.GetScale(), after.GetScale(), tolerance * Math::Max(before.GetScale(), after.GetScale()))))
                return false;
            if (m_Commands.size() == 128)
                m_Commands.erase(m_Commands.begin());
            m_Commands.push_back({ id, before });
            return true;
        }

        // The receiver returns false for deleted objects; continue to the next live command.
        // 삭제된 객체를 건너뛰며 직전 TRS를 복구한다.
        template<class Apply>
        bool Undo(Apply&& apply)
        {
            while (!m_Commands.empty())
            {
                const Command command = m_Commands.back();
                m_Commands.pop_back();
                if (apply(command.Id, command.Before))
                    return true;
            }
            return false;
        }

    private:
        struct Command { uint64_t Id; SceneTransform Before; };
        std::vector<Command> m_Commands;
    };
}
