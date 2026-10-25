#include "CutCommon.hlsli"
// 주변 9셀과 실제 거리로 끝점의 유일한 상대를 찾는다. 해시 충돌은 좌표 검사로 거른다.
uint Match(uint endpoint,uint count)
{
    float2 p=asfloat(Ring.Load2((endpoint/2)*128+24+(endpoint&1)*8));
    int2 cell=int2(floor(p/Epsilon));uint match=0xffffffff, hits=0;
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)
    {
        int2 neighbor=cell+int2(x,y);
        uint hash=(asuint(neighbor.x)*73856093u ^ asuint(neighbor.y)*19349663u)&HashMask;
        uint head=Ring.Load(HashOffset+hash*4),steps=0;
        while(head!=0 && steps++<count*2)
        {
            uint other=head-1;head=Ring.Load(HashLinksOffset+other*4);
            if(other/2==endpoint/2)continue;
            float2 q=asfloat(Ring.Load2((other/2)*128+24+(other&1)*8));
            if(all(int2(floor(q/Epsilon))==neighbor)&&dot(p-q,p-q)<=Epsilon*Epsilon)
            {match=other;hits++;}
        }
    }
    return hits==1?match:0xffffffff;
}
// 자기 교차·접촉하는 비인접 변은 단순 다각형용 cap 알고리즘에 넘기지 않는다.
uint Crossing(uint i,uint count)
{
    float2 a=Point2(i),b=asfloat(Ring.Load2(i*128+32));
    uint risk=0;
    for(uint j=0;j<count;j++)
    {
        if(i==j||Ring.Load(j*128+108)==0)continue;
        float2 c=Point2(j),d=asfloat(Ring.Load2(j*128+32));
        if(any(max(a,b)<min(c,d)-Epsilon)||any(max(c,d)<min(a,b)-Epsilon))continue;
        if(min(min(length(a-c),length(a-d)),min(length(b-c),length(b-d)))<=Epsilon)continue;
        float x=Cross2(b-a,c-a),y=Cross2(b-a,d-a),z=Cross2(d-c,a-c),w=Cross2(d-c,b-c);
        float eps=Epsilon*max(length(b-a),length(d-c));
        bool separated=(x>eps&&y>eps)||(x<-eps&&y<-eps)||(z>eps&&w>eps)||(z<-eps&&w<-eps);
        if(separated)continue;
        bool proper=((x>eps&&y<-eps)||(x<-eps&&y>eps))&&((z>eps&&w<-eps)||(z<-eps&&w>eps));
        if(proper)return 4;
        risk|=2; // 허용오차 안의 근접/평행 접촉은 완전 교차로 단정하지 않는다.
    }
    return risk;
}
// 활성 선분만 연결하고 접촉 후보는 고립시켜 경계 전파에서 제외한다.
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    uint i=id.x,count=min(Meta.Load(4),MaxSegments);if(i>=count)return;
    if(count>4096||Ring.Load(i*128+108)==0)
    {
        Ring.Store(i*128+40,i);Ring.Store(i*128+44,i);Ring.Store(i*128+48,i);
        Ring.Store(i*128+52,1);Ring.Store(i*128+56,i);return;
    }
    uint a=Match(i*2,count),b=Match(i*2+1,count);
    uint bad=(a==0xffffffff||b==0xffffffff||(a&1)!=1||(b&1)!=0)?1:0;
    bad|=Crossing(i,count);
    uint next=b==0xffffffff?i:b/2,prev=a==0xffffffff?i:a/2;
    Ring.Store(i*128+40,next);Ring.Store(i*128+44,prev);
    Ring.Store(i*128+48,i);Ring.Store(i*128+52,bad);Ring.Store(i*128+56,next);
    Ring.Store(i*128+80,1);
}
