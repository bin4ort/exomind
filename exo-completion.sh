#!/usr/bin/env bash
# Tab completion for exomind stack modules
# Usage: source exo-completion.sh

# List of exomind stack modules
EXO_MODULES="exomind exosched exoflow exodoc exoqms exocrawl exocontext exokit"

# Tab completion for exomind commands
_exo_completion() {
    local cur="${BASH_CUR% }"
    local prev="${BASH_CUR% }"

    # Get the command name
    local cmd="${BASH_CUR%% *}"
    local rest="${BASH_CUR#* }"

    # If no command, show all available commands
    if [[ "$cmd" == "" ]]; then
        for m in $EXO_MODULES; do
            if [[ -x "$m" ]]; then
                echo "$m"
            fi
        done
        return
    fi

    # Complete commands for specific modules
    case "$cmd" in
        exomind)
            if [[ "$rest" == "" ]]; then
                echo "--host --port --data --token --tokens --keys --rate-limit --log-level --replicate --mcp --update --body --help --version --completion keys"
            elif [[ "$rest" == "--"* ]]; then
                echo "--host --port --data --token --tokens --keys --rate-limit --log-level --replicate --mcp --update --body --help --version --completion"
            elif [[ "$rest" == "keys" ]]; then
                echo "add list remove"
            fi
            break
        esac
}

# Register the completion handler
for cmd in $EXO_MODULES; do
    if [[ -x "$cmd" ]]; then
        register_completion "$cmd" _exo_completion
    fi
done
