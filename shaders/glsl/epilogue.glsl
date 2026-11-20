
void main()
{
    vec2 pos = vec2(gl_FragCoord.x, res.y - gl_FragCoord.y);
    gl_FragColor = ps_main(pos);
}
