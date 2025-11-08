# 🐚 Custom Shell in C++

## 📘 Objective
A simple shell implemented in C++ that executes system commands, manages foreground/background processes, supports redirection, piping, and basic job control.

## 📅 Development Progress
| Day | Features Implemented |
|-----|------------------------|
| 1 | Input parsing and tokenization |
| 2 | Basic command execution (`execvp`) |
| 3 | Foreground & background process handling (`&`) |
| 4 | Piping (`|`) and redirection (`<`, `>`, `>>`) |
| 5 | Job control (`jobs`, `fg`, `bg`) and signal handling |

## 🧰 Requirements
- Linux / macOS terminal  
- `g++` compiler

## 🏃 Run
```bash
g++ -std=c++17 mini-shell.cpp -o minishell
./minishell
