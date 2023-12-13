struct vs_input_t
{
	float4 position : POSITION;
	float4 color : COLOR;
};

struct ps_input_t
{
	float4 position : SV_POSITION;
	float4 color : COLOR;
};

cbuffer cbuffer0 : register(b0)
{
	float4x4 cbuf_mvp;
}

ps_input_t vs(vs_input_t input)
{
	ps_input_t output;

	output.position = mul(cbuf_mvp, input.position);
	output.color = input.color;

	return output;
}

float4 ps(ps_input_t input) : SV_TARGET
{
	return input.color;
}