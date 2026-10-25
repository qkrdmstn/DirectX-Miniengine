#include "CutCommon.hlsli"
// 양쪽에 실제 입력 표면이 있는 분할의 성공한 loop만 cap으로 출력한다.
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    if((Meta.Load(20)&7)!=7)return;
    uint i=id.x,count=min(Meta.Load(4),MaxSegments);if(i>=count)return;
    if(Ring.Load(i*128+108)==0)return;
    uint leader=Ring.Load(i*128+48),ignored;
    uint boundaryFlags=Ring.Load(i*128+52);
    if(boundaryFlags!=0)Meta.InterlockedOr(28,boundaryFlags,ignored);
    if((boundaryFlags&5)!=0){Meta.InterlockedAdd(8,1,ignored);return;}
    if(Ring.Load(leader*128+84)==0){Meta.InterlockedAdd(24,1,ignored);Meta.InterlockedAdd(8,1,ignored);return;}
    if(Ring.Load(i*128+76)==0)return;
    uint3 corners=Ring.Load3(i*128+64);
    Vertex a=(Vertex)0,b=(Vertex)0,c=(Vertex)0;
    a.Origin=b.Origin=c.Origin=uint4(0xffffffff,leader,i,0);
    a.P=Point(corners.x);b.P=Point(corners.y);c.P=Point(corners.z);
    // ProjectU/V의 3D 방향은 component-local에서 직교하지 않아도 평면 법선의 방향을 보존한다.
    if(length(cross(b.P-a.P,c.P-a.P))<=Epsilon*Epsilon)return;
    float3 normal=SafeNormal(Plane.xyz);
    if(dot(cross(b.P-a.P,c.P-a.P),normal)>0){Vertex temp=b;b=c;c=temp;}
    a.N=b.N=c.N=-normal;a.T=b.T=c.T=float4(SafeNormal(ProjectU.xyz),-1);
    a.UV0=a.UV1=Project(a.P);b.UV0=b.UV1=Project(b.P);c.UV0=c.UV1=Project(c.P);
    EmitTriangle(0,Sections-1,a,b,c);
    a.N=b.N=c.N=normal; a.T.w=b.T.w=c.T.w=1;
    EmitTriangle(1,Sections-1,a,c,b);
    Meta.InterlockedAdd(16,1,ignored);
}
