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
#include <dirent.h>
#include <limits.h>

//alguns sistemas nao definem o PATH_MAX no limits.h
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

//flag global para erros silenciosos 
int silent_errors = 0;
//falg global para verbose
int verbose = 0;
//flag global para mostrar apenas mudanças
int changes_only = 0;
//flag global para aplicar o modo recursivamente
int recursive = 0;
//flag global para proteger a raiz '/' na recursao (o padrao do chmod eh nao proteger)
int preserve_root = 0;
//flags globais do --reference, que copia o modo de outro arquivo em vez de usar o MODE
int use_reference = 0;
mode_t reference_mode = 0;
//umask do processo, usada quando o modo simbolico nao diz a classe (ex: +x em vez de a+x)
mode_t umask_value = 0;
/*
ligado quando o modo veio na forma de opcao (chmod -x arq), que eh onde a umask pode
surpreender - nesse caso o chmod confere o resultado e avisa
*/
int diagnose_surprises = 0;

/*
mascaras das classes, do mesmo jeito que o GNU separa:
o u fica com o setuid, o g com o setgid e o o com o sticky
*/
#define MASK_U (S_ISUID | S_IRWXU)
#define MASK_G (S_ISGID | S_IRWXG)
#define MASK_O (S_ISVTX | S_IRWXO)
#define MASK_ALL 07777

//os bits de uma permissao nas tres classes de uma vez
#define BITS_R (S_IRUSR | S_IRGRP | S_IROTH)
#define BITS_W (S_IWUSR | S_IWGRP | S_IWOTH)
#define BITS_X (S_IXUSR | S_IXGRP | S_IXOTH)

/*
le a umask do processo
nao existe syscall so pra consultar, entao zera, guarda o valor antigo e devolve ele pro lugar
*/
mode_t get_umask(void) {
    mode_t value = umask(0);
    umask(value);
    return value;
}

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
calcula o modo novo aplicando uma clausula de cada vez (MODE[,MODE]...) por cima do modo atual
o is_dir importa pro X e pra regra de nao mexer em setuid/setgid de diretorio
a umask vem por parametro pq o chamador precisa calcular tambem como seria sem ela
assume que o mode_str ja foi aprovado pelo is_symbolic_mode
*/
mode_t parse_symbolic(const char *mode_str, mode_t current_mode, int is_dir, mode_t umask_bits) {
    mode_t new_mode = current_mode & MASK_ALL;
    int i = 0;

    while (mode_str[i] != '\0') {
        //classes atingidas pela clausula, 0 quando nao foram escritas (ai vale a umask)
        mode_t affected = 0;

        while (is_ugoa(mode_str[i])) {
            if (mode_str[i] == 'u') affected |= MASK_U;
            if (mode_str[i] == 'g') affected |= MASK_G;
            if (mode_str[i] == 'o') affected |= MASK_O;
            if (mode_str[i] == 'a') affected |= MASK_ALL;
            i++;
        }

        //a mesma clausula pode ter varias operacoes, tipo u+x+w ou ug+-x
        while (is_operator(mode_str[i])) {
            char op = mode_str[i];
            mode_t value = 0;
            mode_t target = affected;
            mode_t mentioned;
            mode_t omit_change;
            mode_t preserved;
            int copy_existing = 0;
            int x_if_any_x = 0;
            int numeric = 0;

            i++;

            if (mode_str[i] >= '0' && mode_str[i] <= '7') {
                //forma numerica [-+=][0-7]+, que atinge todos os bits e nao passa pela umask
                numeric = 1;

                while (mode_str[i] >= '0' && mode_str[i] <= '7') {
                    value = (value << 3) | (mode_t)(mode_str[i] - '0');
                    i++;
                }
                target = MASK_ALL;
            } else if (mode_str[i] == 'u' || mode_str[i] == 'g' || mode_str[i] == 'o') {
                //copia as permissoes de outra classe, tipo g=u
                copy_existing = 1;

                if (mode_str[i] == 'u') value = S_IRWXU;
                if (mode_str[i] == 'g') value = S_IRWXG;
                if (mode_str[i] == 'o') value = S_IRWXO;
                i++;
            } else {
                //r, w, x, X, s e t
                while (is_permission(mode_str[i])) {
                    if (mode_str[i] == 'r') value |= BITS_R;
                    if (mode_str[i] == 'w') value |= BITS_W;
                    if (mode_str[i] == 'x') value |= BITS_X;
                    if (mode_str[i] == 'X') x_if_any_x = 1;
                    if (mode_str[i] == 's') value |= S_ISUID | S_ISGID;
                    if (mode_str[i] == 't') value |= S_ISVTX;
                    i++;
                }
            }

            /*
            o que a clausula pediu na mao, calculado antes de resolver o X e a copia
            a forma numerica conta como se tivesse pedido todos os bits, por isso ela
            zera o setuid/setgid de diretorio igual o octal puro faz
            */
            if (numeric) {
                mentioned = MASK_ALL;
            } else {
                mentioned = target ? (target & value) : value;
            }

            //em diretorio o setuid/setgid so muda se foi pedido explicitamente
            omit_change = (is_dir ? (mode_t)(S_ISUID | S_ISGID) : 0) & ~mentioned;

            if (copy_existing) {
                //isola os bits que a classe de origem tem e espalha pras tres classes
                value &= new_mode;

                if (value & BITS_R) value |= BITS_R;
                if (value & BITS_W) value |= BITS_W;
                if (value & BITS_X) value |= BITS_X;
            }

            if (x_if_any_x) {
                //o X so liga o execute se ja tem algum execute ligado ou se eh diretorio
                if (is_dir || (new_mode & BITS_X)) {
                    value |= BITS_X;
                }
            }

            //sem classe explicita eh a umask que decide o que pode ser mexido
            if (target == 0) {
                target = ~umask_bits;
            }
            value &= target;

            if (op == '=') {
                //com classe explicita preserva o que ficou de fora, sem classe limpa tudo
                preserved = ((affected == 0 && !numeric) ? 0 : ~target) | omit_change;
                new_mode = (new_mode & preserved) | value;
            } else if (op == '+') {
                new_mode |= value;
            } else {
                new_mode &= ~value;
            }

            new_mode &= MASK_ALL;
        }

        //pula a virgula e vai pra proxima clausula
        if (mode_str[i] == ',') {
            i++;
        }
    }

    return new_mode;
}


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
    mode_t final_mode = 0;
    mode_t old_mode;
    //modo que sairia se a umask nao entrasse na conta, so pro aviso de ambiguidade
    mode_t naive_mode = 0;
    int umask_ambiguous = 0;
    char old_text[10];
    char new_text[10];
    char naive_text[10];
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
    final_mode = old_mode;

    if (use_reference) {
        // com --reference nem precisa parsear nada, o modo ja veio pronto do RFILE
        final_mode = reference_mode;
    } else if (is_octal_mode(mode_str)) {
        // modo octal eh facil pq sobrescreve o modo entao eh so parsear
        final_mode = parse_octal(mode_str);
    } else {
        /* 
        parsing simbolico precisa do modo atual do arq para calcular o novo pro chmod
        */
        int is_dir = S_ISDIR(file_status.st_mode) ? 1 : 0;

        final_mode = parse_symbolic(mode_str, old_mode, is_dir, umask_value);

        /*
        modo escrito na forma de opcao (-x, -r, ...) nao diz a classe, entao quem decide
        o alcance eh a umask e o resultado pode nao ser o que o comando parece pedir
        o chmod aplica do mesmo jeito, mas avisa e termina com falha
        */
        if (diagnose_surprises) {
            naive_mode = parse_symbolic(mode_str, old_mode, is_dir, 0);

            //so reclama se a umask deixou o modo MAIS permissivo do que o comando sugere
            umask_ambiguous = (final_mode & ~naive_mode) != 0;
        }
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

    //o aviso sai depois da linha do verbose e nao eh silenciado pelo -f
    if (umask_ambiguous) {
        mode_verbose(final_mode, new_text);
        mode_verbose(naive_mode, naive_text);
        fprintf(stderr, "chmod: %s: new permissions are %s, not %s\n", filepath, new_text, naive_text);
        return -1;
    }

    return 0;
}

/*
aplica o chmod no caminho e, se for diretorio, desce no conteudo dele
o GNU vai de cima pra baixo (primeiro o diretorio, depois o que tem dentro)
*/
int apply_chmod_recursive(const char *mode_str, const char *path) {
    struct stat path_status;
    struct stat child_status;
    DIR *directory;
    struct dirent *entry;
    char child_path[PATH_MAX];
    int result;

    result = apply_chmod(mode_str, path);

    /*
    lstat em vez de stat pq link simbolico pra diretorio nao deve ser percorrido
    se nem deu pra ler ou nao eh diretorio, acabou o trabalho aqui
    */
    if (lstat(path, &path_status) == -1 || !S_ISDIR(path_status.st_mode)) {
        return result;
    }

    directory = opendir(path);
    if (directory == NULL) {
        if (!silent_errors) {
            fprintf(stderr, "chmod: cannot read directory '%s': %s\n", path, strerror(errno));
        }
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        //pula o proprio diretorio e o pai, senao a recursao nunca termina
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        //monta o caminho do filho e confere se caberia no buffer
        if (snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name) >= (int)sizeof(child_path)) {
            if (!silent_errors) {
                fprintf(stderr, "chmod: cannot access '%s/%s': %s\n", path, entry->d_name, strerror(ENAMETOOLONG));
            }
            result = -1;
            continue;
        }

        //o chmod nunca mexe na permissao de link simbolico que ele achou na recursao
        if (lstat(child_path, &child_status) == 0 && S_ISLNK(child_status.st_mode)) {
            continue;
        }

        if (apply_chmod_recursive(mode_str, child_path) != 0) {
            result = -1;
        }
    }

    closedir(directory);
    return result;
}

/*
Flags
*/

/*
checa se o argumento eh uma abreviacao valida de uma opcao longa
(o GNU aceita prefixo desde que nao fique ambiguo, tipo --rec para --recursive)
min_len eh o menor prefixo aceito, sem contar o '--'
*/
int is_long_option(const char *argument, const char *option, int min_len) {
    size_t len;

    if (argument[0] != '-' || argument[1] != '-') {
        return 0;
    }

    argument += 2;
    len = strlen(argument);

    if (len < (size_t)min_len || len > strlen(option)) {
        return 0;
    }

    return strncmp(argument, option, len) == 0;
}

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

/* Flag recursive */
/* --r e --re sao ambiguos entre --recursive e --reference */
int is_ambiguous_recursive_reference_option(const char *argument) {
    return strcmp(argument, "--r") == 0 || strcmp(argument, "--re") == 0;
}

int is_recursive_option(const char *argument) {
    return strcmp(argument, "-R") == 0 || is_long_option(argument, "recursive", 3);
}

/* Flag reference */
/*
devolve o RFILE de um --reference=RFILE (vale pras abreviacoes --ref=, --refe=, ...)
ou NULL se o argumento nao for essa opcao
*/
const char *reference_option_value(const char *argument) {
    const char *equals = strchr(argument, '=');
    char name[32];
    size_t len;

    if (equals == NULL) {
        return NULL;
    }

    len = (size_t)(equals - argument);
    if (len >= sizeof(name)) {
        return NULL;
    }

    memcpy(name, argument, len);
    name[len] = '\0';

    if (!is_long_option(name, "reference", 3)) {
        return NULL;
    }

    return equals + 1;
}

/* Flags preserve-root e no-preserve-root */
int is_preserve_root_option(const char *argument) {
    return is_long_option(argument, "preserve-root", 1);
}

int is_no_preserve_root_option(const char *argument) {
    return is_long_option(argument, "no-preserve-root", 1);
}

/*
checa se o caminho eh a raiz do sistema ('/', '//', ...) para o failsafe do --preserve-root
*/
int is_root_path(const char *path) {
    int i;

    if (path[0] != '/') {
        return 0;
    }

    for (i = 1; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            return 0;
        }
    }
    return 1;
}

//permitir comandos agrupados, como -fc
int is_option_group(const char *argument) {
    int i;
    //tem que começar com - e não pode começar com --
    if (argument[0] != '-' || argument[1] == '-' || argument[1] == '\0') {
        return 0;
    }

    //todas as letras depois do - devem ser opções curtas
    for (i = 1; argument[i] != '\0'; i++) {
        if (argument[i] != 'f' && argument[i] != 'v' && argument[i] != 'c' && argument[i] != 'R') {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    const char *mode_str = NULL;
    const char *reference_value;
    int exit = EXIT_SUCCESS;
    int argument_index = 1;
    int first_file;
    int end_of_options = 0;

    //a umask eh necessaria pro modo simbolico sem classe, tipo +x
    umask_value = get_umask();
    
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

        if (is_ambiguous_recursive_reference_option(argv[argument_index])) {
            fprintf(stderr,"chmod: option '%s' is ambiguous; possibilities: \n'--recursive' '--reference'\nTry 'chmod --help' for more information.\n", argv[argument_index]);
            return EXIT_FAILURE;
        }

        if (is_option_group(argv[argument_index])) {
            int i;

            for (i = 1; argv[argument_index][i] != '\0'; i++) {
                char option = argv[argument_index][i];

                if (option == 'f') {
                    silent_errors = 1;
                }
                if (option == 'v') {
                    verbose = 1;
                    changes_only = 0;
                }
                if (option == 'c') {
                    changes_only = 1;
                    verbose = 0;
                }
                if (option == 'R') {
                    recursive = 1;
                }
            }
            argument_index++;
            continue;
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

        if (is_recursive_option(argv[argument_index])) {
            recursive = 1;
            argument_index++;
            continue;
        }

        if (is_preserve_root_option(argv[argument_index])) {
            preserve_root = 1;
            argument_index++;
            continue;
        }

        if (is_no_preserve_root_option(argv[argument_index])) {
            preserve_root = 0;
            argument_index++;
            continue;
        }

        //--reference=RFILE: o modo vem das permissoes do RFILE
        reference_value = reference_option_value(argv[argument_index]);

        //--reference RFILE: o RFILE vem separado, no argumento seguinte
        if (reference_value == NULL && is_long_option(argv[argument_index], "reference", 3)) {
            if (argument_index + 1 >= argc) {
                fprintf(stderr, "chmod: option '--reference' requires an argument\nTry 'chmod --help' for more information.\n");
                return EXIT_FAILURE;
            }
            argument_index++;
            reference_value = argv[argument_index];
        }

        if (reference_value != NULL) {
            struct stat reference_status;

            //o RFILE eh sempre dereferenciado se for link simbolico, por isso stat e nao lstat
            if (stat(reference_value, &reference_status) == -1) {
                fprintf(stderr, "chmod: failed to get attributes of '%s': %s\n", reference_value, strerror(errno));
                return EXIT_FAILURE;
            }

            use_reference = 1;
            reference_mode = reference_status.st_mode & 07777;
            argument_index++;
            continue;
        }

        /*
        o getopt_long do GNU reserva o '--' pra opcao longa, entao '--' sozinho encerra
        as opcoes e qualquer outro '--algo' que chegou aqui eh opcao invalida, nao modo
        (por isso 'chmod --t-r arq' eh erro mesmo sendo um modo simbolico valido)
        */
        if (argv[argument_index][0] == '-' && argv[argument_index][1] == '-') {
            if (argv[argument_index][2] == '\0') {
                argument_index++;
                end_of_options = 1;
                break;
            }

            fprintf(stderr, "chmod: unrecognized option '%s'\nTry 'chmod --help' for more information.\n", argv[argument_index]);
            return EXIT_FAILURE;
        }

        //se não for uma flag, então é o modo e sai do loop
        break;
    }

    //verifica se tem o minimo de argumentos
    if (argument_index >= argc) {
        fprintf(stderr, "chmod: missing operand\nTry 'chmod --help' for more information.\n");
        return EXIT_FAILURE;
    }
    
    if (use_reference) {
        //com --reference não existe operando de modo, o primeiro operando já é arquivo
        first_file = argument_index;
    } else {
        mode_str = argv[argument_index];
        first_file = argument_index + 1;

        //modo na forma de opcao (-x, -r, ...) e nao depois do '--' eh o caso de avisar
        diagnose_surprises = (mode_str[0] == '-' && !end_of_options);

        //verifica se tem pelo menos um arquivo depois do modo
        if (first_file >= argc) {
            fprintf(stderr,"chmod: missing operand after '%s'\nTry 'chmod --help' for more information.\n",mode_str);
            return EXIT_FAILURE;
        }
        //chmod modo inválido
        if(!is_symbolic_mode(mode_str) && !is_octal_mode(mode_str)){
            invalid_mode(mode_str);
        }
    }
    

    /*
    flags implementadas (parseadas no while lá em cima):
    -v, --verbose: printa o que ta sendo feito
    -R, --recursive: aplica o modo recursivamente a todos os arquivos e diretórios
    -c, --changes: mostra as mudanças que estão sendo feitas
    -f, --silent, --quiet: ignora erros e continua executando
    --reference=RFILE: copia o modo do RFILE em vez de usar o MODE
    --preserve-root / --no-preserve-root: failsafe da raiz na recursao
    -h, --help: mostra a ajuda
    */

   for (int i = first_file; i <= argc - 1; i++) {
        //failsafe: com -R e --preserve-root a raiz nao pode ser alterada
        if (recursive && preserve_root && is_root_path(argv[i])) {
            fprintf(stderr, "chmod: it is dangerous to operate recursively on '%s'\nchmod: use --no-preserve-root to override this failsafe\n", argv[i]);
            exit = EXIT_FAILURE;
            continue;
        }

        if (recursive) {
            if (apply_chmod_recursive(mode_str, argv[i]) != 0) {
                exit = EXIT_FAILURE;
            }
        } else if (apply_chmod(mode_str, argv[i]) != 0) {
        exit = EXIT_FAILURE;
    }
   }

    return exit;
}
