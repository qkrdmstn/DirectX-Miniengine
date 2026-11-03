#pragma once

namespace Viewer
{
    class Scene;
    // 두 모델의 컴포넌트 독립 마스크와 패스별 GPU 출력을 검사한다.
    void RunSkeletalMaskChecks(Scene& scene);
}
