#pragma once

#include "VectorMath.h"
#include <cstdint>
#include <cmath>
#include <vector>

namespace Viewer
{
    // One completed drag is one command.  IDs survive deletion without retaining objects.
    class TransformHistory
    {
    public:
        bool Record(uint64_t id, const Math::UniformTransform& before, const Math::UniformTransform& after)
        {
            using namespace DirectX;
            const XMVECTOR tolerance = XMVectorReplicate(0.000001f);
            const float a = before.GetScale(), b = after.GetScale();
            const bool sameRotation = XMVector4NearEqual(before.GetRotation(), after.GetRotation(), tolerance) ||
                XMVector4NearEqual(before.GetRotation(), -after.GetRotation(), tolerance);
            if (id == 0 || (XMVector3NearEqual(before.GetTranslation(), after.GetTranslation(), tolerance) &&
                sameRotation && std::abs(a - b) <= 0.000001f * (a > b ? a : b)))
                return false;
            if (m_Commands.size() == 128)
                m_Commands.erase(m_Commands.begin());
            m_Commands.push_back({ id, before });
            return true;
        }

        // The receiver returns false for deleted objects; continue to the next live command.
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
        struct Command { uint64_t Id; Math::UniformTransform Before; };
        std::vector<Command> m_Commands;
    };
}
