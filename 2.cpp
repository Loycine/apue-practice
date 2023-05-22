#include <pthread.h>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>

struct ComputeData
{
    int x;
};

void* compute(void* data)
{
    sleep(5);

    ComputeData* compute_ptr = (ComputeData*)data;
    int x = compute_ptr->x;
    int result = 2 * x;
    printf("%d\n", result);
    free(compute_ptr);
    pthread_exit((void *)0);
}

int main()
{
    int x;
    pthread_t ntid;
    while(scanf("%d", &x) != EOF)
    {
        ComputeData* compute_ptr = (ComputeData*) malloc(sizeof(ComputeData));
        compute_ptr->x = x;
        pthread_create(&ntid, NULL, compute, (void *)compute_ptr);
    }
}