typedef struct
{
    float v[4];
} float32x4_t;

float32x4_t f(float x, float y)
{
    float32x4_t xv;
    float32x4_t yv;
    float32x4_t result;
    __asm__("dup %0.4s, %1.s[0]\n" : "=w"(xv) : "w"(x));
    __asm__("dup %0.4s, %1.s[0]\n" : "=w"(yv) : "w"(y));

    __asm__("fadd %0.4s, %1.4s, %2.4s\n" : "=w"(result) : "w"(xv), "w"(yv));

    return result;
}

int main()
{
    f(10.0f, 20.0f);
}
