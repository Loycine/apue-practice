#include <cstdio>
#include <cstdlib>

#include <iostream>
using namespace std;

int main(int argc, char* argv[])
{
    int buffer_size = atoi(argv[1]);
    char* input_file = argv[2];
    char* output_file = argv[3];

    FILE* input = fopen(input_file, "rb");
    FILE* output = fopen(output_file, "wb");

    int n;
    char data[buffer_size + 5];
    while((n=fread(data, sizeof(char), buffer_size, input)) > 0)
    {
        fwrite(data, sizeof(char), n, output);
    }

    fclose(input);
    fclose(output);
}
