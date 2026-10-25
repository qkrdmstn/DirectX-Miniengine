#include "CutCommon.hlsli"
cbuffer BoundaryPass : register(b1) { uint Phase; };
// 모서리 중점과 경계 끝점을 해시 셀로 바꾼다.
int2 Cell(float2 p) { return int2(floor(p/Epsilon)); }
// 실제 좌표 검사와 함께 사용할 해시를 계산한다.
uint Hash(int2 cell) { return (asuint(cell.x)*73856093u ^ asuint(cell.y)*19349663u)&HashMask; }
// 두 끝점 쌍이 같은 모서리인지 방향과 무관하게 확인한다.
bool SameEdge(float2 a,float2 b,float2 c,float2 d)
{
    return (length(a-c)<=Epsilon&&length(b-d)<=Epsilon)||(length(a-d)<=Epsilon&&length(b-c)<=Epsilon);
}
// 평면 위 모서리에 양쪽 표면이 하나씩 붙은 경우만 실제 절단 경계로 유지한다.
bool Resolve(uint i,uint count)
{
    uint kind=Ring.Load(i*128+92);
    if(kind==1)return true;
    if(kind!=2)return false;
    float2 a=Point2(i),b=asfloat(Ring.Load2(i*128+32));
    int2 cell=Cell((a+b)*0.5);uint positives=0,negatives=0;
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)
    {
        int2 neighbor=cell+int2(x,y);
        uint head=Ring.Load(HashOffset+Hash(neighbor)*4),steps=0;
        while(head!=0&&steps++<count)
        {
            uint other=head-1;head=Ring.Load(HashLinksOffset+other*4);
            float2 c=Point2(other),d=asfloat(Ring.Load2(other*128+32));
            if(all(Cell((c+d)*0.5)==neighbor)&&SameEdge(a,b,c,d))
            {
                uint side=Ring.Load(other*128+92);
                positives+=side==2?1u:0u;negatives+=side==4?1u:0u;
            }
        }
    }
    return positives==1&&negatives==1;
}
// 후보 해시·양측 판정·해시 초기화·활성 끝점 해시를 독립 dispatch로 수행한다.
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    uint endpoint=id.x, count=min(Meta.Load(4),MaxSegments);
    if(Phase==2){if(endpoint<=HashMask)Ring.Store(HashOffset+endpoint*4,0);return;}
    if(Phase==1)
    {
        if(endpoint>=count)return;
        bool active=Resolve(endpoint,count);
        Ring.Store(endpoint*128+108,active?1u:0u);
        if(active)
        {
            float3 a=asfloat(Ring.Load3(endpoint*128)),b=asfloat(Ring.Load3(endpoint*128+12));
            float3 r0=mul(LocalToRectangle,float4(a,1)).xyz,r1=mul(LocalToRectangle,float4(b,1)).xyz;
            if(any(abs(r0.xz)>float2(HalfWidth,HalfHeight)+Epsilon)||any(abs(r1.xz)>float2(HalfWidth,HalfHeight)+Epsilon))Fail(1);
            uint ignored;Meta.InterlockedOr(20,4,ignored);
        }
        return;
    }
    if(Phase==0)
    {
        if(endpoint>=count||Ring.Load(endpoint*128+92)==1)return;
        float2 midpoint=(Point2(endpoint)+asfloat(Ring.Load2(endpoint*128+32)))*0.5;
        if(any(abs(floor(midpoint/Epsilon))>2e9)){Fail(8);return;}
        uint previous;Ring.InterlockedExchange(HashOffset+Hash(Cell(midpoint))*4,endpoint+1,previous);
        Ring.Store(HashLinksOffset+endpoint*4,previous);return;
    }
    if(endpoint>=count*2)return;
    uint segment=endpoint/2, end=endpoint&1;
    if(Ring.Load(segment*128+108)==0)return;
    float2 p=asfloat(Ring.Load2(segment*128+24+end*8));
    float2 cellFloat=floor(p/Epsilon);
    if(any(abs(cellFloat)>2e9)){Fail(8);return;}
    int2 cell=int2(cellFloat);
    uint hash=(asuint(cell.x)*73856093u ^ asuint(cell.y)*19349663u)&HashMask;
    uint previous;Ring.InterlockedExchange(HashOffset+hash*4,endpoint+1,previous);
    Ring.Store(HashLinksOffset+endpoint*4,previous);
}
