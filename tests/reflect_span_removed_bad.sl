// Verifies the old reflect span API is not public.
import "reflect"

const OLD = reflect.span_of(OLD)
