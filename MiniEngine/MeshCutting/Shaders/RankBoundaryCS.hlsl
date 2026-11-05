#include "CutCommon.hlsli"

cbuffer Pass : register(b1) {
    uint Commit;
};

// 루프마다 대표 선분을 하나 지정해서 루프 ID로 설정 + 각 정점에 기록된 bad를 루프 전체 정점에 전달
// -> 읽기/쓰기 dispatch를 분리한 pointer jumping 알고리즘으로 상태를 전파
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    uint segmentCount = min(Meta.Load(4), MaxSegments);
    if (i >= segmentCount)
        return;
    
    uint a = i * 128; // 내 선분 슬롯의 시작 주소
    
    // Rings 구조:
    // +48 / +52 / +56 : 대표 / bad / 점프 포인터
    // +96 / +100 / +104 : 대표 / bad / 점프 포인터의 임시 공간
    
    // 대표 선분, bad 값, 다음의 다음 점프 포인터 값을 계산해서 임시 버퍼에 저장
    if (Commit == 0)
    {
        uint j = Ring.Load(a + 56) * 128; // 내 점프 포인터가 가리키는 선분의 슬롯 주소
        Ring.Store(a + 96, min(Ring.Load(a + 48), Ring.Load(j + 48))); // 대표 선분은 루프 내의 선분 번호가 가장 작은 선분
        Ring.Store(a + 100, Ring.Load(a + 52) | Ring.Load(j + 52)); // bad 값을 or 연산으로 전파
        Ring.Store(a + 104, Ring.Load(j + 56)); // 다음의 점프 포인터 값도 계산
    }
    // 임시 버퍼의 값을 실제 칸으로 옮기기
    else
    {
        Ring.Store(a + 48, Ring.Load(a + 96)); // 대표 선분
        Ring.Store(a + 52, Ring.Load(a + 100)); // bad
        Ring.Store(a + 56, Ring.Load(a + 104)); // 점프 포인터
    }
}
