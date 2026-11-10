/*
chmod-reborn.c
Implementação própria do chmod em C usando chamada de sistema POSIX chmod()

deixar sem nome para nao vazar na internet
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
        if (i >= 4 || mode_str[i] < '0' || mode_str[i] > '7') {
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
        //pra ficar igual a mensagem de erro real, se quiser
        fprintf(stderr, "chmod: invalid mode: ‘%s’\nTry 'chmod --help' for more information.\n", mode_str);
        exit(EXIT_FAILURE);
    }
    return (mode_t)val;
}

int is_ugoa(char c) {
    return c == 'u' || c == 'g' || c == 'o' ||  c == 'a';
}

int is_permission(char c) {
    return c == 'r' || c == 'w' || c == 'x' || c == 'X' || c == 's' || c == 't';
}

int is_operator(char c) {
    return c == '+' || c == '-' || c == '=';
}

/*
checa se uma string representa um modo simbólico válido
*/
int is_symbolic_mode(const char *mode_str) {
    int i = 0;

    if (mode_str[0] == '\0')
        return 0;

    //loop para tratar as vírgulas (chmod [OPTION]... MODE[,MODE]... FILE...)
    while (mode_str[i] != '\0') {
        //lê os modos ugoa, que são opcionais 
        while (is_ugoa(mode_str[i])) {
            i++;
        }

        //verifica se tem um +, - ou = depois de ugoa ou no começo
        if (!is_operator(mode_str[i])) {
            return 0;}

        //permite várias operações
        while (is_operator(mode_str[i])) {
            i++;

            //Caso g=u, o+g ou u-o.
            if (mode_str[i] == 'u' ||mode_str[i] == 'g' || mode_str[i] == 'o') {
                i++;

                //Depois pode ter outro operador ou uma vírgula ou acabar a string
                if (mode_str[i] != '\0' && mode_str[i] != ',' && !is_operator(mode_str[i])) {
                    return 0;
                }
            } else {
                //r, w, x, X, s e t
                while (is_permission(mode_str[i])) {
                    i++;
                }
            }
        }

        //verifica se acabou a string ou se tem uma vírgula para o próximo modo
        if (mode_str[i] == '\0') {
            return 1;
        }

        //verifica se tem uma vírgula para o próximo modo
        if (mode_str[i] == ',') {
            i++;

            //verifica se tem algo depois da vírgula
            if (mode_str[i] == '\0') {
                return 0;
            }
        } else {
            //se não tem vírgula, então tinha que ser o fim da string
            return 0;
        }
    }

    return 1;
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
    const char *mode_str;
    int i;
    int exit = EXIT_SUCCESS;
    
    //verifica argumentos de entrada - é obrigatório informar o modo e pelo menos um arquivo
    //trata os erros de forma similar ao chmod

    //chmod sem argumentos
    if (argc == 1) {
        fprintf(stderr,"chmod: missing operand\n" "Try 'chmod --help' for more information.\n");
        return EXIT_FAILURE;
    }

    //chmod com apenas um argumento (chmod x, chmod +x, chmod a.txt)
    if (argc == 2) {
        fprintf(stderr,"chmod: missing operand after '%s'\n""Try 'chmod --help' for more information.\n", argv[1]);
        return EXIT_FAILURE;
    }

    mode_str = argv[1];
    //chmod modo inválido
    if(!is_symbolic_mode(mode_str) && !is_octal_mode(mode_str) {
        fprintf(stderr,"chmod: invalid mode: '%s'\n""Try 'chmod --help' for more information.\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* diretório ou arquivo informados não foram encontrados
    if()
        fprintf(stderr, "chmod: cannot access '%s': No such file or directory\n", argv[2]);
        return EXIT_FAILURE;
    } */

    
    
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
    int exit = EXIT_SUCCESS;

    /*
    ainda nao ta implementado 2+ arquivos, implementar um loop para percorrer do argv2 ate argv argc-1
    */
    if (apply_chmod(mode_str, argv[2]) != 0) {
        exit = EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
