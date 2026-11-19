cbuffer Parameters : register(b0)
{
    float4 Plane;
    float4x4 LocalToRectangle;
    float4x4 LocalToWorld;
    float Epsilon, HalfWidth, HalfHeight;
    uint Triangles;
    uint Draws, DepthOffset, IndexOffset, Mode;
    uint NodeCount, Influence, HasState, Padding1;
};
struct DrawInput
{
    uint VbOffset, Stride, IbOffset, Index32;
    uint StartIndex, BaseVertex, Triangles, PoseOffset;
    uint FirstTriangle, MaskOffset, Flags, SectionId;
    uint DrawId, JointCount, VertexCount, Reserved1;
};
ByteAddressBuffer Source : register(t0);
ByteAddressBuffer SourcePose : register(t1);
StructuredBuffer<DrawInput> Inputs : register(t2);
StructuredBuffer<uint> Mask : register(t3);
StructuredBuffer<uint> State : register(t4);
RWByteAddressBuffer Adjacent : register(u6); // patch id별 인접 제거 삼각형 수
RWByteAddressBuffer Data : register(u0);
RWByteAddressBuffer Sources : register(u1);
RWByteAddressBuffer Meta : register(u2); // counts 0..8, local AABB 12..32, source world X 36..40
RWByteAddressBuffer Scan : register(u3); // inclusive prefix, crossing flag
RWByteAddressBuffer Votes : register(u4); // NodeId별 uint64 (lo, hi)
RWByteAddressBuffer Chosen : register(u5);
groupshared uint Prefix[64];
groupshared uint2 Counts[64];

// 그룹의 선택·교차·미포함 합계를 Hillis-Steele inclusive scan으로 계산한다.
uint3 ScanTotals(uint lane, uint3 value)
{
    // 범위 밖 lane도 0을 기록하고 모든 lane이 초기화를 마칠 때까지 기다린다.
    Prefix[lane]=value.x;
    Counts[lane]=value.yz;
    GroupMemoryBarrierWithGroupSync();

    // 이전 단계 값을 모두 읽은 뒤 덮어쓰고 다음 단계 전에 동기화한다.
    [unroll] for(uint offset=1;offset<64;offset*=2)
    {
        uint3 previous=0;
        if(lane>=offset) previous=uint3(Prefix[lane-offset],Counts[lane-offset]);
        GroupMemoryBarrierWithGroupSync();
        Prefix[lane]+=previous.x;
        Counts[lane]+=previous.yz;
        GroupMemoryBarrierWithGroupSync();
    }

    // 각 lane의 누적 합계를 반환한다.
    return uint3(Prefix[lane],Counts[lane]);
}

// 원본 draw의 R16/R32 인덱스와 baseVertex를 해석한다.
uint Index(DrawInput s, uint i)
{
    // RAW load는 DWORD 정렬 주소를 사용한다.
    uint at=s.StartIndex+i;
    uint word=Source.Load(s.IbOffset+(s.Index32?at*4:(at/2)*4));
    return (s.Index32?word:((word>>((at&1)*16))&65535))+s.BaseVertex;
}
// PreparePose와 같은 정규화 weight로 선택 팔레트의 영향을 판정한다.
bool Selected(DrawInput s, uint i)
{
    // skin이 없는 section은 선택 본의 영향을 받지 않는다.
    if (!(s.Flags&256) || (Influence && Chosen.Load(0)==0xffffffff)) return false;
    uint4 p=Source.Load4(s.VbOffset+i*s.Stride+s.Stride-16);
    uint4 joints=uint4(p.x&65535,p.x>>16,p.y&65535,p.y>>16);
    float4 weights=float4(p.z&65535,p.z>>16,p.w&65535,p.w>>16);
    weights/=max(dot(weights,1),1);
    bool selected=false;
    [unroll] for(uint k=0;k<4;k++)
        if(joints[k]<s.JointCount && weights[k]>0) selected|=Influence ? Mask[s.MaskOffset+joints[k]]==Chosen.Load(0) : Mask[s.MaskOffset+joints[k]]!=0;
    return selected;
}
// CPU ClipCoplanarSegmentToCutPlaneRectangle과 같은 닫힌 구간 겹침을 검사한다.
bool Overlaps(float3 a, float3 b)
{
    // 평면 공간 Y 허용 오차와 XZ slab 규칙을 그대로 적용한다.
    a=mul(LocalToRectangle,float4(a,1)).xyz; b=mul(LocalToRectangle,float4(b,1)).xyz;
    if(any(!isfinite(a))||any(!isfinite(b))||abs(a.y)>1e-5||abs(b.y)>1e-5) return false;
    float2 p=a.xz, delta=b.xz-a.xz, halfSize=float2(HalfWidth,HalfHeight);
    float lo=0,hi=1;
    [unroll] for(uint k=0;k<2;k++)
    {
        if(delta[k]==0) { if(abs(p[k])>halfSize[k]) return false; }
        else
        {
            float x=(-halfSize[k]-p[k])/delta[k],y=(halfSize[k]-p[k])/delta[k];
            lo=max(lo,min(x,y)); hi=min(hi,max(x,y));
            if(lo>hi) return false;
        }
    }
    return true;
}
// 엄격한 양쪽 부호와 유한 사각형을 통과하는 교차 선분을 검사한다.
bool Crossing(float3 a, float3 b, float3 c)
{
    // eps 안의 꼭짓점은 평면으로 투영하여 기준 함수와 일치시킨다.
    float3 p[3]={a,b,c},hits[3]; float d[3];
    [unroll] for(uint k=0;k<3;k++)
    {
        d[k]=dot(Plane,float4(p[k],1));
        if(abs(d[k])<=Epsilon) { p[k]-=Plane.xyz*(d[k]/dot(Plane.xyz,Plane.xyz)); d[k]=0; }
    }
    if(min(d[0],min(d[1],d[2]))>=-Epsilon||max(d[0],max(d[1],d[2]))<=Epsilon) return false;

    // 평면 위 꼭짓점과 부호 반전 변에서 두 교점을 모은다.
    uint count=0;
    [unroll] for(uint i=0;i<3;i++)
    {
        uint j=(i+1)%3;
        if(d[i]==0) hits[count++]=p[i];
        if(d[i]*d[j]<0) hits[count++]=p[i]+(p[j]-p[i])*(d[i]/(d[i]-d[j]));
    }
    return count==2 && Overlaps(hits[0],hits[1]);
}
// cutter의 packed tangent/normal 형식을 만든다.
uint PackFrame(float4 v)
{
    // UNORM 10:10:10:2로 양자화한다.
    uint4 a=uint4(round(saturate(v*.5+.5)*float4(1023,1023,1023,3)));
    return a.x|(a.y<<10)|(a.z<<20)|(a.w<<30);
}
// 유효하지 않은 법선 대신 cutter와 같은 기본 법선을 쓴다.
float3 SafeNormal(float3 n)
{
    // 영벡터의 정규화로 NaN이 생기지 않게 한다.
    return dot(n,n)>1e-20?normalize(n):float3(0,1,0);
}
// float를 AABB 원자 min/max용 정렬 가능한 정수로 바꾼다.
uint Ordered(float x)
{
    // 음수 구간은 비트를 반전하고 양수는 부호 비트를 뒤집는다.
    uint b=asuint(x); return b&0x80000000?~b:b^0x80000000;
}
// 투표·선택·병렬 prefix·추출과 기존 제거 패치의 인접 집계를 실행한다.
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 64비트 투표를 비교하고 동점이면 작은 NodeId를 보존한다.
    uint t=id.x+id.y*(65535*64);
    if(Mode==4)
    {
        if(t!=0) return;
        uint selected=0xffffffff;
        uint2 best=0;
        for(uint node=0;node<NodeCount;node++)
        {
            uint2 vote=Votes.Load2(node*8);
            if(vote.y>best.y || (vote.y==best.y && vote.x>best.x))
            {
                best=vote;
                selected=node;
            }
        }
        Chosen.Store(0,selected);
        return;
    }

    // 블록 안 prefix와 세 종류의 블록 합계를 계산한다.
    if(Mode==1)
    {
        // 초과 그룹과 마지막 블록의 빈 lane은 0으로 scan에 참여한다.
        uint lane=t%64,b=t/64,blocks=(Triangles+63)/64;
        uint2 flags=0;
        if(t<Triangles) flags=Scan.Load2(t*8);
        uint3 total=ScanTotals(lane,uint3(flags,flags.y*(1-flags.x)));

        // 삼각형 영역과 블록 영역의 쓰기 경계를 각각 제한한다.
        if(t<Triangles) Scan.Store(t*8,total.x);
        if(lane==63 && b<blocks) Scan.Store3(Triangles*8+b*16,total);
        return;
    }

    // 한 그룹이 연속 블록 구간의 합계를 scan해 전역 offset과 총계를 만든다.
    if(Mode==6)
    {
        // lane마다 연속 구간을 맡아 세 종류의 합계를 먼저 읽는다.
        uint lane=t%64,blocks=(Triangles+63)/64;
        uint count=(blocks+63)/64;
        uint begin=lane*count,end=min(begin+count,blocks);
        uint3 sum=0;
        for(uint b=begin;b<end;b++) sum+=Scan.Load3(Triangles*8+b*16);

        // 구간 inclusive 합계에서 자신의 합계를 빼 exclusive 시작점을 얻는다.
        uint3 total=ScanTotals(lane,sum);
        uint offset=total.x-sum.x;

        // 구간을 앞에서부터 순회하며 블록 선택 합계를 exclusive offset으로 바꾼다.
        for(uint b=begin;b<end;b++)
        {
            uint address=Triangles*8+b*16;
            uint selected=Scan.Load(address);
            Scan.Store(address,offset);
            offset+=selected;
        }

        // 마지막 lane이 빈 구간이어도 그룹 전체 총계를 한 번 기록한다.
        if(lane==63) Meta.Store3(0,total);
        return;
    }

    // 앞 블록의 합계를 더해 원본 삼각형 순서의 전역 inclusive prefix를 완성한다.
    if(Mode==7)
    {
        // 첫 블록은 이미 완성되어 있으며 초과 lane은 읽기와 쓰기를 모두 건너뛴다.
        uint b=t/64;
        if(t<Triangles && b!=0)
            Scan.Store(t*8,Scan.Load(t*8)+Scan.Load(Triangles*8+b*16));
        return;
    }
    if(t>=Triangles) return;

    // section/draw와 현재 pose의 같은 원본 꼭짓점 번호를 읽는다.
    uint sId=0;
    while(sId+1<Draws && t>=Inputs[sId+1].FirstTriangle) sId++;
    DrawInput s=Inputs[sId]; uint triangleId=t-s.FirstTriangle;
    uint3 indices=uint3(Index(s,triangleId*3),Index(s,triangleId*3+1),Index(s,triangleId*3+2));
    // 제거 삼각형마다 새 패치와 공유하는 원본 정점이 있으면 소유 패치에 한 번 집계한다.
    if(Mode==5)
    {
        if(!(State[t]&0x80000000u) || Meta.Load(0)==0) return;
        for(uint d=0;d<Draws;d++)
        {
            DrawInput other=Inputs[d];
            if(s.VbOffset!=other.VbOffset || s.Stride!=other.Stride) continue;
            for(uint j=0;j<other.Triangles;j++)
            {
                uint global=other.FirstTriangle+j;
                if(Scan.Load(global*8)==(global?Scan.Load((global-1)*8):0)) continue;
                uint3 v=uint3(Index(other,j*3),Index(other,j*3+1),Index(other,j*3+2));
                if(any(indices.x==v)||any(indices.y==v)||any(indices.z==v))
                {
                    uint ignored;
                    Adjacent.InterlockedAdd((State[t]&65535u)*4,1,ignored);
                    return;
                }
            }
        }
        return;
    }

    if(Mode==3)
    {
        // 추출 카운터와 같은 유한 사각형 교차 판정을 사용한다.
        if(!(s.Flags&256) || any(indices>=s.VertexCount)) return;
        if(!Crossing(asfloat(SourcePose.Load3(s.PoseOffset+indices.x*40)),
            asfloat(SourcePose.Load3(s.PoseOffset+indices.y*40)),asfloat(SourcePose.Load3(s.PoseOffset+indices.z*40)))) return;

        // 정규화 weight를 배율 65535로 반올림하고 원자 덧셈의 carry를 별도 누적한다.
        [unroll] for(uint corner=0;corner<3;corner++)
        {
            uint4 p=Source.Load4(s.VbOffset+indices[corner]*s.Stride+s.Stride-16);
            uint4 joints=uint4(p.x&65535,p.x>>16,p.y&65535,p.y>>16);
            uint4 weights=uint4(p.z&65535,p.z>>16,p.w&65535,p.w>>16);
            uint sum=max(weights.x+weights.y+weights.z+weights.w,1u);
            [unroll] for(uint slot=0;slot<4;slot++)
            {
                if(joints[slot]>=s.JointCount || weights[slot]==0) continue;
                uint node=Mask[s.MaskOffset+joints[slot]];
                if(node>=NodeCount) continue;
                uint weight=(weights[slot]*65535u+sum/2)/sum,previous;
                Votes.InterlockedAdd(node*8,weight,previous);
                if(previous>0xffffffffu-weight) Votes.InterlockedAdd(node*8+4,1);
            }
        }
        return;
    }
    if(Mode==0)
    {
        // 선택 여부와 무관하게 원본 전체 posed 정점의 월드 X 폭을 집계한다.
        [unroll] for(uint corner=0;corner<3;corner++)
        {
            float3 p=asfloat(SourcePose.Load3(s.PoseOffset+indices[corner]*40));
            uint x=Ordered(mul(LocalToWorld,float4(p,1)).x),ignored;
            Meta.InterlockedMin(36,x,ignored); Meta.InterlockedMax(40,x,ignored);
        }

        // 제거 상태를 선택 조건에 더해 prefix와 모든 출력에 함께 반영한다.
        bool available=!HasState || !(State[t]&0x80000000u);
        bool selected=available && (Selected(s,indices.x)||Selected(s,indices.y)||Selected(s,indices.z));
        bool crossing=available && Crossing(asfloat(SourcePose.Load3(s.PoseOffset+indices.x*40)),
            asfloat(SourcePose.Load3(s.PoseOffset+indices.y*40)),asfloat(SourcePose.Load3(s.PoseOffset+indices.z*40)));
        Scan.Store2(t*8,uint2(selected,crossing)); return;
    }

    // 누적값이 증가한 삼각형만 고유 출력 슬롯에 기록한다.
    uint prefix=Scan.Load(t*8),previous=t?Scan.Load((t-1)*8):0;
    if(prefix==previous) return;
    uint outputTriangle=prefix-1;
    Sources.Store3(outputTriangle*12,uint3(s.SectionId,s.DrawId,triangleId));

    // single draw의 정점을 packed VB·RAW40·depth VB·R32 IB에 기록한다.
    [unroll] for(uint corner=0;corner<3;corner++)
    {
        uint i=indices[corner],v=outputTriangle*3+corner,a=s.VbOffset+i*s.Stride,p=s.PoseOffset+i*40;
        float3 position=asfloat(SourcePose.Load3(p)),normal=SafeNormal(asfloat(SourcePose.Load3(p+12)));
        float4 tangent=asfloat(SourcePose.Load4(p+24)); tangent.xyz=SafeNormal(tangent.xyz); tangent.w=tangent.w<0?-1:1;
        uint uvAt=a+16+((s.Flags&4)?4:0);
        uint uv0=(s.Flags&8)?Source.Load(uvAt):0,uv1=(s.Flags&16)?Source.Load(uvAt+((s.Flags&8)?4:0)):uv0;
        Data.Store3(v*28,asuint(position)); Data.Store(v*28+12,PackFrame(float4(normal,1)));
        Data.Store3(v*28+16,uint3(PackFrame(tangent),uv0,uv1));
        uint depthStride=(Inputs[0].Flags&64)?16:12;
        Data.Store3(DepthOffset+v*depthStride,asuint(position));
        if(depthStride==16) Data.Store(DepthOffset+v*depthStride+12,uv0);
        Data.Store(IndexOffset+v*4,v);
        [unroll] for(uint axis=0;axis<3;axis++)
        {
            uint ignored; Meta.InterlockedMin(12+axis*4,Ordered(position[axis]),ignored);
            Meta.InterlockedMax(24+axis*4,Ordered(position[axis]),ignored);
        }
    }
}
