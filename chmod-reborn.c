/*
chmod-reborn.c
implementacao propria do chmod em C usando chamada de sistema POSIX chmod()

kitagawa
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
checa se uma string representa um octal valido (define qual o modo de declaracao dos modos)
*/
int is_octal_mode(const char *mode_str) {
    if (*mode_str == '\0') return 0;
    for (int i = 0; mode_str[i] != '\0'; i++) {
        if (mode_str[i] < '0' || mode_str[i] > '7') {
            return 0;
        }
    }
    return 1;
}

/*
parseia uma string octal para um mode_t bitmask pedido pela syscall do chmod (sempre bom ler o manual)
*/
mode_t parse_octal(const char *mode_str) {
    char *endptr;

    // strtol converte string para long em base 8
    // tambem ajuda a checar se a string eh valida, se nao for o endptr vai referenciar o primeiro char nao parseavel
    long val = strtol(mode_str, &endptr, 8);
    
    // tem caracter nao parseavel ou o valor eh negativo ou maior que 07777 (12 bits de perm)
    if (*endptr != '\0' || val < 0 || val > 07777) {
        fprintf(stderr, "Erro: modo octal invalido '%s'\n", mode_str);
        exit(EXIT_FAILURE);
    }
    return (mode_t)val;
}

/*
parser simbolcio
nao implementado
*/
mode_t parse_symbolic(const char *mode_str, mode_t current_mode);

/*
checa se o modo ta declarado como octal
chama o parser corresp
aplica o chmod no arquivo usando syscall POSIX
*/
int apply_chmod(const char *mode_str, const char *filepath) {
    mode_t final_mode;

    if (is_octal_mode(mode_str)) {
        // modo octal eh facil pq sobrescreve o modo entao eh so parsear
        final_mode = parse_octal(mode_str);
    } else {
        /* 
        parsing simbolico precisa do modo atual do arq para calcular o novo pro chmod
        ainda nao implementei
        */
    }

    // chama syscall POSIX para mudar permissoes (nao sei se isso eh roubar k)
    if (chmod(filepath, final_mode) == -1) {
        fprintf(stderr, "ERRO: nao foi possivel mudar as perms de '%s': %s\n", 
                filepath, strerror(errno));
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // confere se tem o minimo de argumentos
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <modo> <arq1> [arq2 ...]\n", argv[0]); //[arq2 ...] nao implementado ainda
        return EXIT_FAILURE;
    }

    /*
    colocar aqui parsing de flag depois
    as flags podem ser (sao 4 vou ver se invento uma nova pra ficar com 5 e ganhar o bonus):
    -v, --verbose: printa o que ta sendo feito
    -R, --recursive: aplica o modo recursivamente a todos os arquivos e diretórios
    -c, --changes: mostra as mudanças que estão sendo feitas
    -f, --force: ignora erros e continua executando
    -h, --help: mostra a ajuda
    */

    const char *mode_str = argv[1];

    /*
    ainda nao ta implementado 2+ arquivos, implementar um loop para percorrer do argv2 ate argv argc-1
    */
    if (apply_chmod(mode_str, argv[2]) != 0) {
        exit_status = EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}