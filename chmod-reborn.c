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

//flag global para erros silenciosos 
int silent_errors = 0;
//falg global para verbose
int verbose = 0;
//flag global para mostrar apenas mudanças
int changes_only = 0;

void invalid_mode(const char *mode_str) {
    fprintf(stderr, "chmod: invalid mode: '%s'\nTry 'chmod --help' for more information.\n", mode_str);
    exit(EXIT_FAILURE);
}

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
        invalid_mode(mode_str);
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
        int has_ugoa = 0;
        //lê os modos ugoa, que são opcionais 
        while (is_ugoa(mode_str[i])) {
            has_ugoa = 1;
            i++;
        }

        //verifica se tem um +, - ou = depois de ugoa ou no começo
        if (!is_operator(mode_str[i])) {
            return 0;}

        //permite várias operações
        //tem que aceitar vários zeros antes do número
        while (is_operator(mode_str[i])) {
            i++;

            //casos do help =755, =0755 (ou +zeros), +755 ou -755
            if (mode_str[i] >= '0' && mode_str[i] <= '7') {
                //evita u=755
                if (has_ugoa)
                    return 0;

                while (mode_str[i] >= '0' && mode_str[i] <= '7') {
                  i++;
            }
            //depois do número, vírgula ou fim da string
            if (mode_str[i] != '\0' && mode_str[i] != ',') {
                    return 0;}
            }
         
            //Caso g=u, o+g ou u-o.
            else if (mode_str[i] == 'u' ||mode_str[i] == 'g' || mode_str[i] == 'o') {
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


/*preenche o texto com as permissões para o verbose */
void mode_verbose(mode_t mode, char text[10]) {
    //dono
    text[0] = (mode & S_IRUSR) ? 'r' : '-';
    text[1] = (mode & S_IWUSR) ? 'w' : '-';
    text[2] = (mode & S_IXUSR) ? 'x' : '-';
    //grupo
    text[3] = (mode & S_IRGRP) ? 'r' : '-';
    text[4] = (mode & S_IWGRP) ? 'w' : '-';
    text[5] = (mode & S_IXGRP) ? 'x' : '-';
    //outros
    text[6] = (mode & S_IROTH) ? 'r' : '-';
    text[7] = (mode & S_IWOTH) ? 'w' : '-';
    text[8] = (mode & S_IXOTH) ? 'x' : '-';

    //Se o bit setuid estiver ativo, ele vai pra posição de execuçãodo dono
    if (mode & S_ISUID) {
        text[2] = (mode & S_IXUSR) ? 's' : 'S';
    }

    //setgid na posição de execução do grupo
    if (mode & S_ISGID) {
        text[5] = (mode & S_IXGRP) ? 's' : 'S';
    }

    //sticky bit na posição de execução de outros
    if (mode & S_ISVTX) {
        text[8] = (mode & S_IXOTH) ? 't' : 'T';
    }

    text[9] = '\0'; }

/*
checa se o modo ta declarado como octal
chama o parser corresp
aplica o chmod no arquivo usando syscall POSIX
*/
int apply_chmod(const char *mode_str, const char *filepath) {
    mode_t final_mode;
    mode_t old_mode;
    char old_text[10];
    char new_text[10];
    struct stat file_status;

    if (stat(filepath, &file_status) == -1) {
        if (!silent_errors) {
            fprintf(stderr, "chmod: cannot access '%s': %s\n", filepath, strerror(errno));
        }
        if (verbose) {
            printf("'%s' could not be accessed\n", filepath); }
    return -1; 
    }


    old_mode = file_status.st_mode & 07777;

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
        if (!silent_errors) {
            //acho que tem que ajustar depois essa mensagem para os casos possíveis de erro
            fprintf(stderr, "chmod: changing permissions of '%s': %s\n", filepath, strerror(errno));
        }
        return -1;
    }

    if (verbose || (changes_only && old_mode != final_mode)) {
        mode_verbose(old_mode, old_text);
        mode_verbose(final_mode, new_text);

        if (old_mode == final_mode) {
            printf("mode of '%s' retained as %04o (%s)\n", filepath, (unsigned int)old_mode, old_text);
        } else {
            printf("mode of '%s' changed from %04o (%s) to %04o (%s)\n", filepath, (unsigned int)old_mode, old_text, (unsigned int)final_mode, new_text);}
        }
    return 0;
}

/*
Flags
*/

int is_ambiguous_verbose_version_option(const char *argument) {
    return strcmp(argument, "--v") == 0 || strcmp(argument, "--ve") == 0 || strcmp(argument, "--ver") == 0;
}

/* Flag help */
int is_help_option(const char *argument) {
    return strcmp(argument, "--h") == 0 || strcmp(argument, "--he") == 0 || strcmp(argument, "--hel") == 0 || strcmp(argument, "--help") == 0;
}

void flag_help(void) {
    fprintf(stdout,
        "Usage: chmod [OPTION]... MODE[,MODE]... FILE...\n"
        "  or:  chmod [OPTION]... OCTAL-MODE FILE...\n"
        "  or:  chmod [OPTION]... --reference=RFILE FILE...\n"
        "\n"
        "Change the mode of each FILE to MODE.\n"
        "With --reference, change the mode of each FILE to that of RFILE.\n"
        "\n"
        "  -c, --changes          like verbose but report only when a change is made\n"
        "  -f, --silent, --quiet  suppress most error messages\n"
        "  -v, --verbose          output a diagnostic for every file processed\n"
        "      --no-preserve-root  do not treat '/' specially (the default)\n"
        "      --preserve-root     fail to operate recursively on '/'\n"
        "      --reference=RFILE  use RFILE's mode instead of specifying MODE values.\n"
        "                         RFILE is always dereferenced if a symbolic link.\n"
        "  -R, --recursive        change files and directories recursively\n"
        "      --help        display this help and exit\n"
        "      --version     output version information and exit\n"
        "\n"
        "Each MODE is of the form '[ugoa]*([-+=]([rwxXst]*|[ugo]))+|[-+=][0-7]+'.\n"
        "\n"
        "GNU coreutils online help: <https://www.gnu.org/software/coreutils/>\n"
        "Report any translation bugs to <https://translationproject.org/team/>\n"
        "Full documentation <https://www.gnu.org/software/coreutils/chmod>\n"
        "or available locally via: info '(coreutils) chmod invocation'\n");
}

/* Flag version */
int is_version_option(const char *argument) {
    return strcmp(argument, "--vers") == 0 || strcmp(argument, "--versi") == 0 || strcmp(argument, "--versio") == 0 || strcmp(argument, "--version") == 0;
}

void flag_version(void) {
    fprintf(stdout,
        "Reference version of reinplemented chmod:\n'"
        "chmod (GNU coreutils) 9.4\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n"
        "\n"
        "License GPLv3+: GNU GPL version 3 or later "
        "<https://gnu.org/licenses/gpl.html>.\n"
        "This is free software: you are free to change and redistribute it.\n"
        "There is NO WARRANTY, to the extent permitted by law.\n"
        "\n"
        "Written by David MacKenzie and Jim Meyering.\n"
        "Reimplemented by G. & P.\n");
}

/* Flag silent/quiet/f*/

int is_silent_option(const char *argument) {
    return strcmp(argument, "-f") == 0 || strcmp(argument, "--silent") == 0 || strcmp(argument, "--quiet") == 0;
}

/* Flag verbose */
int is_verbose_option(const char *argument) {
    return strcmp(argument, "-v") == 0 ||  strcmp(argument, "--verb") == 0 || strcmp(argument, "--verbo") == 0 || strcmp(argument, "--verbose") == 0;
}

/* Flag changes*/
int is_changes_option(const char *argument) {
    return strcmp(argument, "-c") == 0 || strcmp(argument, "--changes") == 0;
}

int main(int argc, char *argv[]) {
    const char *mode_str;
    int exit = EXIT_SUCCESS;
    int argument_index = 1;
    
    //verifica argumentos de entrada - é obrigatório informar o modo e pelo menos um arquivo
    //trata os erros de forma similar ao chmod
    //while para ler opções (flags) antes do modo e dos arquivos
    while (argument_index < argc) {
        if (argc >= 2 && is_help_option(argv[argument_index])) {
            flag_help();
            return EXIT_SUCCESS;
        }

        if (argc >= 2 && is_ambiguous_verbose_version_option(argv[argument_index])) {
            fprintf(stderr,"chmod: option '%s' is ambiguous; possibilities: \n'--verbose' '--version'\nTry 'chmod --help' for more information.\n", argv[argument_index]);
            return EXIT_FAILURE;
        }

        if (argc >= 2 && is_version_option(argv[argument_index])) {
            flag_version();
            return EXIT_SUCCESS;
        }

        if (is_verbose_option(argv[argument_index])) {
            verbose = 1;
            changes_only = 0;
            argument_index++;
            continue;
        }

        if (is_changes_option(argv[argument_index])) {
            changes_only = 1;
            verbose = 0;
            argument_index++;
            continue;
        }


        if (argc >= 2 && is_silent_option(argv[argument_index])) {
            silent_errors = 1;
            argument_index++;
            continue;
        }
        //se não for uma flag, então é o modo e sai do loop
        break;
    }

    //verifica se tem o minimo de argumentos
    if (argument_index >= argc) {
        fprintf(stderr, "chmod: missing operand\nTry 'chmod --help' for more information.\n");
        return EXIT_FAILURE;
    }
    
    mode_str = argv[argument_index];

    //verifica se tem pelo menos um arquivo depois do modo
    if (argument_index + 1 >= argc) {
        fprintf(stderr,"chmod: missing operand after '%s'\nTry 'chmod --help' for more information.\n",mode_str);
        return EXIT_FAILURE;
    }
    //chmod modo inválido
    if(!is_symbolic_mode(mode_str) && !is_octal_mode(mode_str)){
        invalid_mode(mode_str);
    }

    /* diretório ou arquivo informados não foram encontrados
    if()
        fprintf(stderr, "chmod: cannot access '%s': No such file or directory\n", argv[2]);
        return EXIT_FAILURE;
    } */

    
    
    // // confere se tem o minimo de argumentos
    // if (argc < 3) {
    //     fprintf(stderr, "Uso: %s <modo> <arq1> [arq2 ...]\n", argv[0]); //[arq2 ...] nao implementado ainda
    //     return EXIT_FAILURE;
    // }

    /*
    colocar aqui parsing de flag depois
    as flags podem ser (sao 4 vou ver se invento uma nova pra ficar com 5 e ganhar o bonus):
    -v, --verbose: printa o que ta sendo feito
    -R, --recursive: aplica o modo recursivamente a todos os arquivos e diretórios
    -c, --changes: mostra as mudanças que estão sendo feitas
    -f, --force: ignora erros e continua executando
    -h, --help: mostra a ajuda
    */

    

    /*
    ainda nao ta implementado 2+ arquivos, implementar um loop para percorrer do argv2 ate argv argc-1 - OK
    */
   for (int i = argument_index + 1; i <= argc - 1; i++) {
        if (apply_chmod(mode_str, argv[i]) != 0) {
        exit = EXIT_FAILURE;
    }
   }

    return exit;
}
