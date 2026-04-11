sum Option {
  None,
  Some(u32),
}

fn f(x: (Option,)) -> u32 {
  match x {
    (Option::Some(v),) => v,
    _ => u32:0,
  }
}
