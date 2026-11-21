float3 gradeLayer(float3 rgb, float4 rows[3]) {
    if (rows[0].w == 0) return rgb;
    return max(0, float3(dot(rgb, rows[0].xyz), dot(rgb, rows[1].xyz), dot(rgb, rows[2].xyz)));
}
