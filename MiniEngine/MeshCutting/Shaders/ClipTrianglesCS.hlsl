#include "CutCommon.hlsli"

// 유효한 입력 표면의 양쪽 존재를 기록하고 각 삼각형과 단면 선분을 출력한다.
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    uint tri=id.x;
    if(tri>=Triangles)return;
    uint section=0;
    while(section+1<Sections-1 && tri>=Inputs[section+1].FirstTriangle)section++;
    Section s=Inputs[section];
    uint t=tri-s.FirstTriangle;
    uint3 indices=uint3(Index(s,t*3),Index(s,t*3+1),Index(s,t*3+2));
    if(any(indices>=s.VertexCount)){Fail(4);return;}
    Vertex v[3]={ReadVertex(s,indices.x),ReadVertex(s,indices.y),ReadVertex(s,indices.z)};
    [unroll]for(uint vertex=0;vertex<3;vertex++)v[vertex].TriangleId=t;
    float3 area=cross(v[1].P-v[0].P,v[2].P-v[0].P);
    if(dot(area,area)<=1e-24)return;
    uint ignored;
    if(s.Reserved!=0)
    {
        Meta.InterlockedOr(20,1u<<(s.Reserved-1),ignored);
        EmitTriangle(s.Reserved-1,section,v[0],v[1],v[2]);return;
    }
    float d[3]; bool positive=false,negative=false;
    [unroll]for(uint j=0;j<3;j++){d[j]=dot(Plane,float4(v[j].P,1));if(abs(d[j])<=Epsilon)d[j]=0; positive|=d[j]>0;negative|=d[j]<0;}
    // 공면 면은 한쪽에만 유지해 중복 표면을 피한다.
    if(!positive&&!negative){EmitTriangle(0,section,v[0],v[1],v[2]);return;}
    [unroll]for(uint side=0;side<2;side++)
    {
        Vertex poly[4]; uint n=0;
        [unroll]for(uint k=0;k<3;k++)
        {
            uint p=(k+2)%3; float a=side==0?d[p]:-d[p],b=side==0?d[k]:-d[k];
            if((a>0&&b<0)||(a<0&&b>0))poly[n++]=IntersectEdge(v[p],v[k],a,b);
            if(b>=0)poly[n++]=v[k];
        }
        for(uint q=1;q+1<n;q++)
            if(dot(cross(poly[q].P-poly[0].P,poly[q+1].P-poly[0].P),cross(poly[q].P-poly[0].P,poly[q+1].P-poly[0].P))>1e-24)
            {
                EmitTriangle(side,section,poly[0],poly[q],poly[q+1]);
                // 실제로 출력한 비공면 표면만 양쪽 존재의 증거로 사용한다.
                if(side==0?positive:negative)Meta.InterlockedOr(20,1u<<side,ignored);
            }
    }
    if(!positive&&!negative)return;
    float3 enter=0,exit=0;bool gotEnter=false,gotExit=false;
    uint kind=1;
    [unroll]for(uint e=0;e<3;e++)
    {
        uint n=(e+1)%3;
        if(d[e]<=0&&d[n]>0){enter=IntersectEdge(v[e],v[n],d[e],d[n]).P;gotEnter=true;}
        if(d[e]>0&&d[n]<=0){exit=IntersectEdge(v[e],v[n],d[e],d[n]).P;gotExit=true;}
    }
    if(!positive||!negative)
    {
        kind=positive?2u:4u;
        [unroll]for(uint edge=0;edge<3;edge++)
            if(d[edge]==0&&d[(edge+1)%3]==0){enter=v[(edge+1)%3].P;exit=v[edge].P;gotEnter=gotExit=true;}
    }
    if(!gotEnter||!gotExit||length(Project(enter)-Project(exit))<=Epsilon)return;
    uint at;Meta.InterlockedAdd(4,1,at);if(at>=MaxSegments){Fail(2);return;}
    Ring.Store3(at*128,asuint(enter));Ring.Store3(at*128+12,asuint(exit));
    Ring.Store2(at*128+24,asuint(Project(enter)));Ring.Store2(at*128+32,asuint(Project(exit)));
    // 1=면 내부 교차, 2=양쪽 중 양의 표면 모서리, 4=음의 표면 모서리 후보다.
    Ring.Store(at*128+92,kind);
}
