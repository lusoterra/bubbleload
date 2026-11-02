# fish completion for bubbleload
#
# Install as /usr/share/fish/vendor_completions.d/bubbleload.fish, or for a
# local build:
#
#     cp completions/bubbleload.fish ~/.config/fish/completions/

function __fish_bubbleload_profiles
    set -l dir "$HOME/.config/bubbleload"
    if set -q XDG_CONFIG_HOME; and test -n "$XDG_CONFIG_HOME"
        set dir "$XDG_CONFIG_HOME/bubbleload"
    end

    for f in $dir/*.conf
        if test -f $f
            string replace -r '.*/' '' -- $f | string replace -r '\.conf$' ''
        end
    end
end

# True while the loader options may still be given, i.e. before BINARY.
function __fish_bubbleload_needs_command
    set -l tokens (commandline -opc)
    set -e tokens[1]

    while set -q tokens[1]
        set -l t $tokens[1]
        if contains -- $t -p --profile -w --rw -r --ro -P --project
            set -e tokens[1]
            if set -q tokens[1]
                set -e tokens[1]
            end
        else if string match -q -- '-*' $t
            set -e tokens[1]
        else
            return 1
        end
    end
    return 0
end

# True when the word under the cursor is BINARY itself.
function __fish_bubbleload_at_command
    __fish_bubbleload_needs_command; or return 1
    set -l tokens (commandline -opc)
    set -l last $tokens[-1]
    if contains -- $last -p --profile -w --rw -r --ro -P --project
        return 1
    end
    return 0
end

complete -c bubbleload -s h -l help -d 'Show this help' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s l -l list -d 'List available profiles and exit' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s y -l yes -d 'Skip confirmation prompts' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s n -l dry-run -d 'Print the bwrap command and exit' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s p -l profile -x -d 'Use profile NAME' \
    -a '(__fish_bubbleload_profiles)' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s w -l rw -x -d 'Extra read-write directory' \
    -a '(__fish_complete_directories)' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s r -l ro -x -d 'Extra read-only directory' \
    -a '(__fish_complete_directories)' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -s P -l project -x -d 'Project directory' \
    -a '(__fish_complete_directories)' \
    -n __fish_bubbleload_needs_command

complete -c bubbleload -s C -l cwd -d 'Expose the current directory' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -l allow-home -d 'Allow exposing $HOME (dangerous)' \
    -n __fish_bubbleload_needs_command
complete -c bubbleload -l dangerous -d 'Alias for --allow-home' \
    -n __fish_bubbleload_needs_command

# BINARY gets command names, the program's own arguments get plain file names.
complete -c bubbleload -f -n __fish_bubbleload_at_command \
    -a '(__fish_complete_command)'
complete -c bubbleload -F -n 'not __fish_bubbleload_at_command'
