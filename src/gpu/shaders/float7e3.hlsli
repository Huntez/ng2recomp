// 2_10_10_10_FLOAT (7e3) color as the EDRAM stores it: clamped to
// [0, 31.875] (NaN and -0 to 0, as Xenia's NClamp) and rounded to 7e3, from
// Xenia's PreClampedFloat32To7e3 / Float7e3To32.
float Round7e3(float value)
{
    uint f = (value > 0.0) ? asuint(min(value, 31.875)) : 0u;
    uint denormal = ((f & 0x7FFFFFu) | 0x800000u) >> min(125u - (f >> 23), 24u);
    uint biased = f < 0x3E800000u ? denormal : f - (124u << 23);
    uint f10 = ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
    uint exponent = f10 >> 7, mantissa = f10 & 0x7Fu;
    return exponent == 0 ? float(mantissa) / 512.0
                         : (1.0 + float(mantissa) / 128.0) * exp2(float(exponent) - 3.0);
}

// Color and 2-bit alpha of one 7e3 sample.
float4 Round7e3(float4 color)
{
    return float4(Round7e3(color.r), Round7e3(color.g), Round7e3(color.b),
                  round(saturate(color.a) * 3.0) / 3.0);
}
