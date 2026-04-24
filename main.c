#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>

#define MAX_LENGTH 100
#define MAX_PIPE 5

volatile sig_atomic_t child_flag = 0;
/*コマンドの状態*/
typedef struct{
    int pipe;
    int bg;
    int output;
}command_state;

/*プロセスの情報管理*/
typedef struct process{
    int index;
    int pid;
    int pgid;
    command_state state;
    struct process* prev;
    struct process* next;
}process;

static process *head;
static process *tail;

/*シグナルハンドラー(SIGINT)*/
void signal_child(int signal){
    child_flag = 1;
}

/*シグナルの初期化*/
void signal_init(){
    struct sigaction sa_chld;
    sa_chld.sa_handler = signal_child;
    sigemptyset(&sa_chld.sa_mask); //handlerの実行中に他のsignalを受け取る
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP; //SA_RESTART：シグナル割り込み後自動で再会。SA_NOCLDSTOP：プロセスの停止ではSIGCHILD送らない
    if(sigaction(SIGCHLD, &sa_chld, NULL) < 0){
        fprintf(stderr,"SIGCHLD error");
        exit(1);
    }

    struct sigaction sa_ign;
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;

    sigaction(SIGINT,  &sa_ign, NULL);
    sigaction(SIGQUIT, &sa_ign, NULL);
    sigaction(SIGTSTP, &sa_ign, NULL);
    sigaction(SIGTTIN, &sa_ign, NULL);  
    sigaction(SIGTTOU, &sa_ign, NULL);  
}

/*プロセスの初期化*/
void proc_init(process *proc){
    proc->index  = 0;
    proc->pid    = 0;
    proc->pgid   = 0;
    proc->state.pipe   = 0;
    proc->state.bg     = 0;
    proc->state.output = 0;
}

/*リストの初期化*/
int list_init(){
    head = (process*)malloc(sizeof(process));
    tail = (process*)malloc(sizeof(process));
    if(head == NULL || tail == NULL){
        fprintf(stderr,"failed to malloc\n");
        return -1;
    }
    head->prev = tail;
    head->next = tail;
    tail->next = head;
    tail->prev = head;
    proc_init(head);
    proc_init(tail);
    return 1;
}

/*リスト構造の追加(バッググラウンドのプロセスを管理)*/
void list_append(process *proc){
    proc->prev = tail->prev;
    tail->prev->next = proc;
    proc->next = tail;
    tail->prev = proc;
    proc->index = (proc->prev->index + 1);
}

/*リストからプロセスを削除*/
void list_remove(process *proc){
    process *dummy = proc->next;
    while(dummy != tail){
        dummy->index--;
        dummy = dummy->next;
    }
    if(proc->prev) proc->prev->next = proc->next;
    if(proc->next) proc->next->prev = proc->prev;
    free(proc);
}

/*ユーザからの標準入力*/
int input_command(char cmd[MAX_LENGTH]){
    if(fgets(cmd, MAX_LENGTH, stdin) == NULL){
        if(errno == EINTR) return -1;
        fprintf(stderr, "failed to fgets\n");
        return -1;
    }
    if(strchr(cmd, '\n') == NULL){
        printf("error: command too long\n");
        int c; 
        while((c = getchar()) != '\n' && c != EOF); //stdinの残ってるfgetsから溢れた分を捨てる
        return -1;
    }
    cmd[strcspn(cmd, "\n")] = '\0';
    if(cmd[0] == '\0') return -1;
    
    return 1;
}

/*終了したプロセスを探し表示する*/
void search_finish_process(){
    process *current = head->next;
    while(current != tail){
        process *next = current->next;
        int status;
        if(waitpid(current->pid, &status, WNOHANG) > 0){
            printf("[+] finish pid %d\n", current->pid);
            list_remove(current);
        }
        current = next;
    }
}

/*jobsコマンド*/
void jobs_cmd(){
    process *dummy = head->next;
    while(dummy != tail){
        printf("[%d] pid %d\n", dummy->index, dummy->pid);
        dummy = dummy->next;
    }
}

/*string型数字からintへ*/
int string_to_numeric(char *str){
    if(*str == '\0') return -1;
    char *dummy = str;
    while(*dummy){
        if(!isdigit((unsigned char)*dummy)) return -1;
        dummy++;
    }
    return atoi(str);
}

/*fgのwait部分*/
void wait_fg(pid_t pgid, int *out_status){
    tcsetpgrp(STDIN_FILENO, pgid);//子プロセスにTTY渡す
    int status = 0;
    while(waitpid(-pgid, &status, WUNTRACED) > 0){
        if(WIFSTOPPED(status)) break;
    }
    if(out_status) *out_status = status;
    tcsetpgrp(STDIN_FILENO, getpgrp());//シェルにTTY移す
}

/*fgで選ばれたindexを持つプロセス待機*/
int fg_cmd(int index){
    process *dummy = head->next;
    while(dummy != tail){
        if(dummy->index == index){
            printf("fg: pid %d\n", dummy->pid);
            int status;
            wait_fg((pid_t)dummy->pgid, &status);
            if(!WIFSTOPPED(status)){
                //正常でないwait_fg通知の場合
                list_remove(dummy);
                return -1;
            }
            return 1;
        }
        dummy = dummy->next;
    }
    return -1;
}

/*リダイレクション機能 > */
int redirection(const char* filename){
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(fd == -1){ perror("open"); return -1; }
    if(dup2(fd, STDOUT_FILENO) == -1){ perror("dup2"); close(fd); return -1; }
    close(fd);
    return 1;
}

/*execで実行させないプロセス*/
int speecific_command_exec(char* argv[MAX_PIPE][MAX_LENGTH],process* proc){
    if(argv[0][0] == NULL){ return -1; }
    if(strcmp(argv[0][0],"fg") == 0){
        if(argv[0][1] == NULL){ fprintf(stderr,"usage: fg <job id>\n"); return -1; }//fgのみの場合
        if(argv[0][2] != NULL){ fprintf(stderr,"fg: too many arguments\n"); return -1; }//fgの引数が一個以上の時
        int idx;
        if((idx = string_to_numeric(argv[0][1])) < 0){ fprintf(stderr,"fg: invalid id\n"); return -1; }//引数の値が数字が確認
        if(fg_cmd(idx) == -1){ fprintf(stderr,"fg: no such job: %d\n", idx); return -1;}//プロセスをフォアグラウンドにする
        return -1;
    }else if(strcmp(argv[0][0],"jobs") == 0){
        jobs_cmd();
    }else if(strcmp(argv[0][0],"exit") == 0 || strcmp(argv[0][0],"quit") == 0){
        exit(0);
    }
    return 1;
}

/*コマンドを空白単位で区切る*/
int split_command_by_space(char* cmd, char *argv[MAX_PIPE][MAX_LENGTH], process* proc){
    char *token;
    int cmd_idx = 0;
    int arg_idx = 0;
    token = strtok(cmd, " ");

    while(token != NULL){
        if(*token == '|'){
            //パイプ上限のチェック
            if(cmd_idx >= MAX_PIPE){ fprintf(stderr,"over max pipe number\n"); return -1; }
            /*pipeが来た地点までのコマンドを保存*/
            argv[cmd_idx][arg_idx] = NULL;
            cmd_idx++;
            arg_idx = 0;
        } else if(*token == '>'){
            proc->state.output = arg_idx;
        } else if(*token == '&'){
            proc->state.bg = 1;
        } else {
            if(arg_idx >= MAX_LENGTH - 1){
                fprintf(stderr,"over max length\n");
                return -1;
            }
            argv[cmd_idx][arg_idx++] = token;
        }
        token = strtok(NULL, " ");
    }
    argv[cmd_idx][arg_idx] = NULL;
    proc->state.pipe = cmd_idx;

    return 1;
}

/*子プロセス作成後、exec*/
void create_process(process *proc, char* argv[][MAX_LENGTH]){
    int initial_fd = STDIN_FILENO;
    int fd[2];
    int pids[MAX_PIPE + 1];
    int cmd_num = proc->state.pipe + 1;
    pid_t pgid = 0;

    for(int i = 0; i < cmd_num; i++){
        int pipe_read_fd = 0;  //次のループで使うread端
        if(i < proc->state.pipe){
            if(pipe(fd) < 0){ perror("pipe"); return; }
            pipe_read_fd = fd[0];  //次の子に渡すread端を先に保存
        }

        pid_t pid = fork();
        if(pid == 0){
            //シグナルをデフォルトに戻す 子プロセスまでSIGINTがSIG_IGNになるから
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            //pgidの設定：最初の子はgetpid()、それ以降は親から受け取ったpgidを使う
            //パイプありのコマンドを一つのプロセスとして認識するため
            pid_t my_pgid = (pgid == 0) ? getpid() : pgid;
            setpgid(0, my_pgid);

            //STDIN をパイプのread端に置き換え
            if(initial_fd != STDIN_FILENO){
                dup2(initial_fd, STDIN_FILENO);
                close(initial_fd);
            }

            //STDOUT をパイプのwrite端に置き換え
            if(i < proc->state.pipe){
                close(fd[0]);
                dup2(fd[1], STDOUT_FILENO);
                close(fd[1]);
            }

            //リダイレクト
            if(proc->state.output != 0 && i == proc->state.pipe){
                char *filename = argv[i][proc->state.output];
                argv[i][proc->state.output] = NULL;
                if(redirection(filename) == -1){ exit(1); }
            }
            execvp(argv[i][0], argv[i]);
            perror(argv[i][0]);
            exit(1);

        } else if(pid > 0){
            pids[i] = pid;
            if(pgid == 0){
                pgid = pid;
                if(proc->state.bg == 0){
                    tcsetpgrp(STDIN_FILENO, pgid);  //親が端末制御を渡す
                }
            }
            setpgid(pid, pgid);

            //前のread端は使い終わったので閉じる
            if(initial_fd != STDIN_FILENO) close(initial_fd);

            //パイプのwrite端は親には不要なので閉じ、read端を次ループへ
            if(i < proc->state.pipe){
                close(fd[1]);
                initial_fd = pipe_read_fd;  //次の子に渡すread端をセット
            }
        } else {
            perror("fork");
            return;
        }
    }

    if(proc->state.bg == 0){
        wait_fg(pgid, NULL);
    } else {
        //pipeの最後のプロセスをpidに入れる。
        proc->pid  = pids[cmd_num - 1];
        proc->pgid = pgid;
        list_append(proc);
        printf("[%d] pid %d\n", proc->index, proc->pid);
    }
}

int main(){
    char cmd[MAX_LENGTH];
    char *argv[MAX_PIPE][MAX_LENGTH];
    if(list_init() == -1){
        exit(1);
    }
    signal_init();

    while(1){
        memset(argv, 0, sizeof(argv));
        int ret = 0;
        if(child_flag == 1){
            //終わったプロセスを探索
            search_finish_process();
            child_flag = 0;
        }

        printf("-> ");
        //コマンド入力
        ret = input_command(cmd);
        if(ret == -1){
            continue;
        }

        process *proc = (process*)malloc(sizeof(process));
        proc_init(proc);

        //入力されたコマンドをスペースで分割
        ret = split_command_by_space(cmd,argv,proc);
        if(ret == -1){
            free(proc);
            continue;
        }
        //execvpに渡さないコマンド実行
        ret = speecific_command_exec(argv,proc);
        if(ret == -1){
            free(proc);
            continue;
        }

        //forkからのexecvp実行
        create_process(proc, argv);
        if(proc->state.bg == 0) free(proc);
    }
}
