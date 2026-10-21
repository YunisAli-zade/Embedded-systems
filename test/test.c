#include <stdio.h>

int main(void)
{
    int v = 255;
    int *p = &v;

    printf("%d\n", p);

    return 0;

}