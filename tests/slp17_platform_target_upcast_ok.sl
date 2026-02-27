import "platform"

fn takes_base() context Context {}

fn takes_core() context Context {
	takes_base()
}
