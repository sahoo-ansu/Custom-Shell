#include <bits/stdc++.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
using namespace std;

// Split by space but keep operators
vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string token;
    istringstream iss(line);
    while (iss >> token) tokens.push_back(token);
    return tokens;
}

// Execute single command
void executeCommand(vector<string> &args) {
    vector<char*> argv;
    for (auto &s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    perror("execvp");
    exit(1);
}

int main() {
    string line;
    cout << "Custom Shell (Day 4) — Pipes & Redirection\n";

    while (true) {
        cout << "$ ";
        getline(cin, line);
        if (line == "exit") break;
        if (line.empty()) continue;

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        // Handle redirection and piping
        vector<vector<string>> commands;
        vector<string> current;
        for (auto &t : tokens) {
            if (t == "|") {
                commands.push_back(current);
                current.clear();
            } else current.push_back(t);
        }
        commands.push_back(current);

        int n = commands.size();
        int pipefd[2], in_fd = 0;

        for (int i = 0; i < n; i++) {
            pipe(pipefd);
            pid_t pid = fork();
            if (pid == 0) {
                dup2(in_fd, 0);
                if (i < n - 1) dup2(pipefd[1], 1);
                close(pipefd[0]);
                executeCommand(commands[i]);
            } else {
                waitpid(pid, nullptr, 0);
                close(pipefd[1]);
                in_fd = pipefd[0];
            }
        }
    }
    return 0;
}
