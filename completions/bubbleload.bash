# bash completion for bubbleload
#
# Install as /usr/share/bash-completion/completions/bubbleload, or source it
# from ~/.bashrc for a local build:
#
#     source /path/to/completions/bubbleload.bash

_bubbleload_profiles()
{
    local dir="${XDG_CONFIG_HOME:-$HOME/.config}/bubbleload"
    local f

    for f in "$dir"/*.conf; do
        [[ -f $f ]] || continue
        f=${f##*/}
        printf '%s\n' "${f%.conf}"
    done
}

_bubbleload()
{
    local cur prev i phase value opt

    COMPREPLY=()
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=
    if (( COMP_CWORD > 0 )); then
        prev=${COMP_WORDS[COMP_CWORD-1]}
    fi

    # Loader options are only accepted before BINARY, so work out what the
    # word under the cursor is: another option, the program itself, or one of
    # the program's own arguments.
    phase=options
    for ((i = 1; i < COMP_CWORD; i++)); do
        case ${COMP_WORDS[i]} in
            --)
                if (( i + 1 >= COMP_CWORD )); then
                    phase=command
                else
                    phase=program
                fi
                break
                ;;
            -p|--profile|-w|--rw|-r|--ro|-P|--project)
                i=$((i + 1))
                ;;
            -?*)
                ;;
            *)
                phase=program
                break
                ;;
        esac
    done

    case $phase in
        program)
            # -o default hands this to bash's plain file completion.
            return 0
            ;;
        command)
            COMPREPLY=( $(compgen -c -- "$cur") )
            return 0
            ;;
    esac

    # --profile=NAME and --rw=DIR style arguments.
    case $cur in
        --profile=*)
            COMPREPLY=( $(compgen -W "$(_bubbleload_profiles)" -- "${cur#*=}") )
            return 0
            ;;
        --rw=*|--ro=*|--project=*)
            opt=${cur%%=*}
            value=${cur#*=}
            while IFS= read -r i; do
                COMPREPLY+=( "$opt=$i" )
            done < <(compgen -d -- "$value")
            return 0
            ;;
    esac

    # The word after an option that takes a value.
    case $prev in
        -p|--profile)
            COMPREPLY=( $(compgen -W "$(_bubbleload_profiles)" -- "$cur") )
            return 0
            ;;
        -w|--rw|-r|--ro|-P|--project)
            COMPREPLY=( $(compgen -d -- "$cur") )
            return 0
            ;;
    esac

    if [[ $cur == -* ]]; then
        COMPREPLY=( $(compgen -W '-h --help -l --list -y --yes -n --dry-run
            -p --profile -w --rw -r --ro -P --project -C --cwd
            --allow-home --dangerous' -- "$cur") )
    else
        COMPREPLY=( $(compgen -c -- "$cur") )
    fi
    return 0
}

complete -o default -F _bubbleload bubbleload
