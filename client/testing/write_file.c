#include <stdio.h>   
#include <stdlib.h>  
#include <time.h> 

/*
 * TODO: 
 * You can add more options to get filename or buffer size (each line).
 * This code is only helpful to know how to generate random characters...
 */
size_t BUF_SIZE = 1 * 1024; // (1 << 10);, BUF_SIZE - 1 for newline 
size_t LINE_SIZE = (1 << 24);

char* gen_sentence() {
    srand((unsigned int)time(NULL));

    char *code_str = (char *)malloc(sizeof(char) * BUF_SIZE);
    int type; 
    for (int i = 0; i < BUF_SIZE; i++) {
        type = rand() % 3;  
        if (type == 0) 
            code_str[i] = '0' + rand() % 10; 
        else if (type == 1) 
            code_str[i] = 'a' + rand() % 26;
        else 
            code_str[i] = 'A' + rand() % 26; 
    }
    code_str[BUF_SIZE] = 0;
    return code_str;
    //printf("%s\n", code_str);

}

int main(int argc, char *argv[])
{
	if(argc != 2) {
		fprintf(stderr, "Usage: %s <file_path>\n", argv[0]);
		exit(1);
	}

	const char *file_path = argv[1];
    FILE *fp = fopen(file_path, "w");

    char *buf;
    buf = gen_sentence();
    if(fp) {
        printf("%d %d\n", BUF_SIZE, LINE_SIZE);
        for (int i = 0; i < LINE_SIZE; i++) {
            fprintf(fp, "%s\n", buf);
        }

    }else
        fprintf(stderr, "Failed to open file\n");

    free(buf);
    fclose(fp);
    return 0;
}
