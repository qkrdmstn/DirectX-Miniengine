cbuffer Parameters : register(b0)
{
    float4 Plane;
    float4x4 LocalToRectangle;
    float Epsilon, HalfWidth, HalfHeight;
    uint Triangles;
    uint Draws, DepthOffset, IndexOffset, Mode;
};
struct DrawInput
{
    uint VbOffset, Stride, IbOffset, Index32;
    uint StartIndex, BaseVertex, Triangles, PoseOffset;
    uint FirstTriangle, MaskOffset, Flags, SectionId;
    uint DrawId, JointCount, Reserved0, Reserved1;
};
ByteAddressBuffer Source : register(t0);
ByteAddressBuffer SourcePose : register(t1);
StructuredBuffer<DrawInput> Inputs : register(t2);
StructuredBuffer<uint> Mask : register(t3);
RWByteAddressBuffer Data : register(u0);
RWByteAddressBuffer Pose : register(u1);
RWByteAddressBuffer Sources : register(u2);
RWByteAddressBuffer Meta : register(u3); // counts 0..8, ordered local AABB 12..32
RWByteAddressBuffer Scan : register(u4); // inclusive prefix, crossing flag

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
    if (!(s.Flags&256)) return false;
    uint4 p=Source.Load4(s.VbOffset+i*s.Stride+s.Stride-16);
    uint4 joints=uint4(p.x&65535,p.x>>16,p.y&65535,p.y>>16);
    float4 weights=float4(p.z&65535,p.z>>16,p.w&65535,p.w>>16);
    weights/=max(dot(weights,1),1);
    bool selected=false;
    [unroll] for(uint k=0;k<4;k++)
        if(joints[k]<s.JointCount && weights[k]>0) selected|=Mask[s.MaskOffset+joints[k]]!=0;
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
// 선택·prefix sum·정적 packed 출력의 세 패스를 같은 입력 계약으로 실행한다.
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 순차 prefix sum으로 원본 삼각형 순서를 보존한다.
    uint t=id.x+id.y*(65535*64);
    if(Mode==1)
    {
        if(t!=0) return;
        uint sum=0,crossing=0,uncovered=0;
        for(uint i=0;i<Triangles;i++)
        {
            uint2 flags=Scan.Load2(i*8);
            sum+=flags.x; crossing+=flags.y; uncovered+=flags.y*(1-flags.x);
            Scan.Store(i*8,sum);
        }
        Meta.Store3(0,uint3(sum,crossing,uncovered));
        return;
    }
    if(t>=Triangles) return;

    // section/draw와 현재 pose의 같은 원본 꼭짓점 번호를 읽는다.
    uint sId=0;
    while(sId+1<Draws && t>=Inputs[sId+1].FirstTriangle) sId++;
    DrawInput s=Inputs[sId]; uint triangleId=t-s.FirstTriangle;
    uint3 indices=uint3(Index(s,triangleId*3),Index(s,triangleId*3+1),Index(s,triangleId*3+2));
    if(Mode==0)
    {
        bool selected=Selected(s,indices.x)||Selected(s,indices.y)||Selected(s,indices.z);
        bool crossing=Crossing(asfloat(SourcePose.Load3(s.PoseOffset+indices.x*40)),
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
        Pose.Store3(v*40,asuint(position)); Pose.Store3(v*40+12,asuint(normal)); Pose.Store4(v*40+24,asuint(tangent));
        [unroll] for(uint axis=0;axis<3;axis++)
        {
            uint ignored; Meta.InterlockedMin(12+axis*4,Ordered(position[axis]),ignored);
            Meta.InterlockedMax(24+axis*4,Ordered(position[axis]),ignored);
        }
    }
}
