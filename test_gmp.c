#include <gmp.h>
#include <stdio.h>

extern int __gmp_fprintf(FILE *, const char *, ...);

int main()
{
    mpz_t n;
    FILE *f;

    // Initialize mpz_t variable
    mpz_init(n);

    // Set value to mpz_t variable
    mpz_set_ui(n, 123);

    // Open a file for writing
    f = fopen("output.txt", "w");
    if (f == NULL)
    {
        fprintf(stderr, "Error opening file.\n");
        return 1;
    }

    // Write value of n to file using gmp_fprintf
    __gmp_fprintf(f, "Number: %Zd\n", n);

    // Close the file
    fclose(f);

    // Clear the mpz_t variable
    mpz_clear(n);

    return 0;
}
