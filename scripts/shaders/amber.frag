/*
 * kmscon example shader: amber phosphor monitor
 *
 * Converts every color on screen into an amber hue whose tone depends on the
 * original brightness: black stays black, dim colors turn deep amber, bright
 * colors turn hot amber.
 *
 * Inputs available to every kmscon fragment shader:
 *   varying vec2 texcoord;        screen texture coordinates (0..1)
 *   uniform sampler2D source;     the rendered console
 *   uniform float time;           seconds since the renderer started
 *   uniform vec2 resolution;      screen size in pixels
 */

precision mediump float;

varying vec2 texcoord;

uniform sampler2D source;
uniform float time;
uniform vec2 resolution;

void main()
{
	vec4 val = texture2D(source, texcoord);
	float brightness = dot(val.rgb, vec3(0.299, 0.587, 0.114));

	/* gamma-lift the darker shades so dim text stays readable */
	float b = pow(brightness, 0.7);

	/* dim amber for dark pixels, hot amber for bright pixels; the red floor
	 * is high enough that even quite dark content stays visible */
	vec3 dim = vec3(0.45, 0.12, 0.02);
	vec3 hot = vec3(1.00, 0.75, 0.03);
	vec3 amber = mix(dim, hot, b);

	/* the very hottest pixels glow a little lighter, but stay a saturated
	 * amber-gold instead of washing out to cream or white */
	float glow = smoothstep(0.95, 1.0, brightness);
	amber = mix(amber, vec3(1.0, 0.85, 0.35), glow * 0.4);

	/* vertical screen-door: alternating dim (60%) and bright (100%) pixel
	 * columns simulating CRT pixel gaps; parity check, no per-pixel sine */
	float door = 1.0 - 0.4 * mod(floor(texcoord.x * resolution.x), 2.0);

	gl_FragColor = vec4(amber * b * door, val.a);
}
