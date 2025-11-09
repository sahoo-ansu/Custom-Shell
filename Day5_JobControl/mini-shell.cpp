// mini-shell.cpp
// Compile: g++ -std=c++17 -Wall -Wextra -O2 -o minishell mini-shell.cpp

#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>

using namespace std;

enum JobState { RUNNING, STOPPED, DONE };
struct Job {
    int id;
    pid_t pgid;
    string cmd;
    JobState state;
};

static vector<Job> jobs;
static int next_job_id = 1;
static pid_t shell_pgid;
static int shell_terminal;
static struct termios shell_tmodes;

// Signal flag for safe reaping in main loop
static volatile sig_atomic_t sigchld_flag = 0;

void mark_sigchld(int) {
    sigchld_flag = 1;
}

// Utilities
int find_job_by_pgid(pid_t pgid) {
    for (size_t i = 0; i < jobs.size(); ++i) if (jobs[i].pgid == pgid) return (int)i;
    return -1;
}
int find_job_by_id(int id) {
    for (size_t i = 0; i < jobs.size(); ++i) if (jobs[i].id == id) return (int)i;
    return -1;
}
void add_job(pid_t pgid, const string &cmd, JobState st) {
    Job j; j.id = next_job_id++; j.pgid = pgid; j.cmd = cmd; j.state = st;
    jobs.push_back(j);
}
void remove_done_jobs() {
    vector<Job> alive;
    for (auto &j : jobs) if (j.state != DONE) alive.push_back(j);
    jobs.swap(alive);
}
void print_jobs() {
    for (auto &j : jobs) {
        string s = (j.state==RUNNING?"Running":(j.state==STOPPED?"Stopped":"Done"));
        cout << '[' << j.id << "] " << s << "\t" << j.cmd << "\n";
    }
}

// Reap children and update job states (safe to call from main loop)
void reap_children_nonblocking() {
    int status;
    pid_t pid;
    while (true) {
        pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break;
        pid_t pg = getpgid(pid);
        if (pg < 0) continue;
        int idx = find_job_by_pgid(pg);
        if (idx < 0) continue;
        if (WIFSTOPPED(status)) {
            jobs[idx].state = STOPPED;
        } else if (WIFCONTINUED(status)) {
            jobs[idx].state = RUNNING;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            jobs[idx].state = DONE;
        }
    }
}

// Tokenizer that respects quotes and special tokens
vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string cur;
    bool in_sq=false, in_dq=false;
    for (size_t i=0;i<line.size();) {
        char c = line[i];
        if (isspace((unsigned char)c) && !in_sq && !in_dq) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            ++i;
        } else if (c=='\'' && !in_dq) { in_sq = !in_sq; ++i; }
        else if (c=='"' && !in_sq) { in_dq = !in_dq; ++i; }
        else if (!in_sq && !in_dq && (c=='|'||c=='<'||c=='>'||c=='&')) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            if (c=='>' && i+1<line.size() && line[i+1]=='>') { tokens.push_back(">>"); i+=2; }
            else { tokens.push_back(string(1,c)); ++i; }
        } else { cur.push_back(c); ++i; }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

struct Command {
    vector<string> argv;
    string infile, outfile;
    bool append = false;
};

// Parse tokens into pipeline and background flag
bool parse_pipeline(const vector<string> &tokens, vector<Command> &pipeline, bool &background) {
    pipeline.clear(); background = false;
    if (tokens.empty()) return false;
    Command cmd;
    for (size_t i=0;i<tokens.size();) {
        string t = tokens[i];
        if (t=="|") {
            if (cmd.argv.empty()) return false;
            pipeline.push_back(cmd);
            cmd = Command();
            ++i;
        } else if (t=="<") {
            if (i+1>=tokens.size()) return false;
            cmd.infile = tokens[i+1]; i+=2;
        } else if (t==">" || t==">>") {
            if (i+1>=tokens.size()) return false;
            cmd.outfile = tokens[i+1]; cmd.append = (t==">>"); i+=2;
        } else if (t=="&" && i==tokens.size()-1) {
            background = true; ++i;
        } else {
            cmd.argv.push_back(t); ++i;
        }
    }
    if (!cmd.argv.empty() || !cmd.infile.empty() || !cmd.outfile.empty())
        pipeline.push_back(cmd);
    return !pipeline.empty();
}

char **make_argv(const vector<string> &v) {
    char **argv = new char*[v.size()+1];
    for (size_t i=0;i<v.size();++i) argv[i] = strdup(v[i].c_str());
    argv[v.size()] = nullptr;
    return argv;
}
void free_argv(char **argv) {
    if (!argv) return;
    for (size_t i=0; argv[i]!=nullptr; ++i) free(argv[i]);
    delete[] argv;
}

// Built-in handling
bool handle_builtin(vector<string> &argv) {
    if (argv.empty()) return false;
    string cmd = argv[0];
    if (cmd=="cd") {
        const char *path = (argv.size()>=2) ? argv[1].c_str() : getenv("HOME");
        if (!path) path = "/";
        if (chdir(path) != 0) perror("cd");
        return true;
    } else if (cmd=="exit") {
        exit(0);
    } else if (cmd=="jobs") {
        remove_done_jobs();
        print_jobs();
        return true;
    } else if (cmd=="fg" || cmd=="bg") {
        if (argv.size()<2) {
            cerr << cmd << ": usage: " << cmd << " %jobid\n";
            return true;
        }
        string s = argv[1];
        if (!s.empty() && s[0]=='%') s = s.substr(1);
        int id = stoi(s);
        int idx = find_job_by_id(id);
        if (idx < 0) { cerr << cmd << ": no such job\n"; return true; }
        pid_t pgid = jobs[idx].pgid;
        if (cmd=="fg") {
            // put to foreground and wait
            tcsetpgrp(shell_terminal, pgid);
            if (kill(-pgid, SIGCONT) < 0 && errno!=ESRCH) perror("kill (SIGCONT)");
            int status;
            waitpid(-pgid, &status, WUNTRACED);
            if (WIFSTOPPED(status)) jobs[idx].state = STOPPED;
            else jobs[idx].state = DONE;
            tcsetpgrp(shell_terminal, shell_pgid);
        } else {
            // bg: continue and leave in background
            if (kill(-pgid, SIGCONT) < 0 && errno!=ESRCH) perror("kill (SIGCONT)");
            jobs[idx].state = RUNNING;
        }
        return true;
    }
    return false;
}

// Spawn child; parent will set pgid and handle terminal
pid_t spawn_command(Command &cmd, pid_t pgid, int in_fd, int out_fd) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        // Child: set its process group
        pid_t mypid = getpid();
        if (pgid == 0) pgid = mypid;
        setpgid(0, pgid);

        // Restore default signals in child
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        // Setup I/O redirection from in_fd/out_fd
        if (in_fd != STDIN_FILENO) { dup2(in_fd, STDIN_FILENO); close(in_fd); }
        if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }

        // File redirections specified in command
        if (!cmd.infile.empty()) {
            int fd = open(cmd.infile.c_str(), O_RDONLY);
            if (fd < 0) { perror("open infile"); _exit(1); }
            dup2(fd, STDIN_FILENO); close(fd);
        }
        if (!cmd.outfile.empty()) {
            int flags = O_WRONLY | O_CREAT | (cmd.append ? O_APPEND : O_TRUNC);
            int fd = open(cmd.outfile.c_str(), flags, 0644);
            if (fd < 0) { perror("open outfile"); _exit(1); }
            dup2(fd, STDOUT_FILENO); close(fd);
        }

        char **argv = make_argv(cmd.argv);
        execvp(argv[0], argv);
        perror("execvp");
        free_argv(argv);
        _exit(127);
    } else {
        // Parent: ensure process group is set
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid); // ignore errors if race
        return pid;
    }
}

// Launch pipeline: create processes, set pgid, manage terminal for foreground
void launch_pipeline(vector<Command> &pipeline, bool background, const string &cmdline) {
    size_t n = pipeline.size();
    int prev_fd = STDIN_FILENO;
    vector<int> fds_to_close;
    pid_t pgid = 0;

    // Create pipes and spawn processes
    for (size_t i = 0; i < n; ++i) {
        int pipefd[2] = {-1,-1};
        if (i+1 < n) {
            if (pipe(pipefd) < 0) { perror("pipe"); return; }
        }
        int out_fd = (i+1<n) ? pipefd[1] : STDOUT_FILENO;
        pid_t pid = spawn_command(pipeline[i], pgid, prev_fd, out_fd);
        if (pid < 0) {
            if (pipefd[0]!=-1) close(pipefd[0]);
            if (pipefd[1]!=-1) close(pipefd[1]);
            return;
        }
        if (pgid == 0) pgid = pid;

        // close parent's copy of fds we no longer need
        if (prev_fd != STDIN_FILENO) close(prev_fd);
        if (out_fd != STDOUT_FILENO) close(out_fd);

        if (pipefd[0] != -1) prev_fd = pipefd[0];
        else prev_fd = STDIN_FILENO;
    }

    // Register job
    add_job(pgid, cmdline, RUNNING);
    int idx = find_job_by_pgid(pgid);

    if (!background) {
        // Put job in foreground: give terminal to job's pgid
        tcsetpgrp(shell_terminal, pgid);

        // Wait for the job to finish or stop
        int status;
        pid_t w;
        do {
            w = waitpid(-pgid, &status, WUNTRACED);
        } while (w > 0 && !(WIFEXITED(status) || WIFSIGNALED(status) || WIFSTOPPED(status)));

        // Update job state based on last status
        if (w > 0) {
            if (WIFSTOPPED(status)) {
                jobs[idx].state = STOPPED;
                cout << "\n[" << jobs[idx].id << "] Stopped\t" << jobs[idx].cmd << "\n";
            } else {
                jobs[idx].state = DONE;
            }
        }

        // Restore shell as terminal owner
        tcsetpgrp(shell_terminal, shell_pgid);
    } else {
        cout << '[' << jobs[idx].id << "] " << pgid << " " << jobs[idx].cmd << "\n";
    }
}

// Initialize shell: process group, terminal settings, signals
void init_shell() {
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();

    // Ignore interactive job-control signals
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN); // shell will forward SIGTSTP to children when needed
    signal(SIGINT, SIG_IGN);  // shell ignores SIGINT and forwards to foreground jobs

    // Put shell in its own pgid
    if (setpgid(shell_pgid, shell_pgid) < 0) {
        // might fail if another pgid already set; ignore
    }
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);

    // SIGCHLD handler sets a flag; actual reaping happens in main loop
    struct sigaction sa;
    sa.sa_handler = mark_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, nullptr);
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    init_shell();

    string line;
    while (true) {
        // If SIGCHLD occurred, reap children and update jobs
        if (sigchld_flag) {
            reap_children_nonblocking();
            sigchld_flag = 0;
        }

        // Print done jobs (once)
        for (auto &j : jobs) {
            if (j.state == DONE) {
                cout << "[" << j.id << "] Done\t" << j.cmd << "\n";
            }
        }
        remove_done_jobs();

        // Prompt
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) cout << cwd << " $ ";
        else cout << "$ ";
        cout.flush();

        if (!getline(cin, line)) {
            cout << "\n";
            break;
        }
        if (line.empty()) continue;

        auto tokens = tokenize(line);
        vector<Command> pipeline;
        bool background = false;
        if (!parse_pipeline(tokens, pipeline, background)) {
            cerr << "Parse error\n";
            continue;
        }

        // If single builtin without redirection/pipe, handle directly
        if (pipeline.size() == 1 && pipeline[0].infile.empty() && pipeline[0].outfile.empty()) {
            vector<string> args = pipeline[0].argv;
            if (handle_builtin(args)) continue;
        }

        // Launch pipeline
        launch_pipeline(pipeline, background, line);
    }

    return 0;
}
