import "platform"
import "platform/cli-libc" as cli

fn takes_base() context platform.Context {}

fn takes_target() context cli.Context {
    takes_base()
}
