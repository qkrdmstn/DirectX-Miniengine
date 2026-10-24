// 깊이 검사를 통과한 평면 선의 불투명 HDR 색을 출력한다.
float4 main(float4 position : SV_Position, float4 color : COLOR) : SV_Target
{
    return color;
}
