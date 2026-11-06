Texture2D<float> SceneDepth : register(t0);
RWByteAddressBuffer PickDepth : register(u0);

cbuffer PickPixel : register(b0)
{
    uint2 Pixel;
};

// 클릭한 픽셀의 장면 깊이를 CPU로 복사할 버퍼에 쓴다.
[numthreads(1, 1, 1)]
void main()
{
    // 깊이의 float 비트를 그대로 보존한다.
    PickDepth.Store(0, asuint(SceneDepth.Load(int3(Pixel, 0))));
}
