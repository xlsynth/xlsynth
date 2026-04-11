sum Never {}

sum S {
  Unit,
  Impossible(Never),
}

fn f(x: S) -> u32 {
  match x {
    S::Unit => u32:0,
  }
}
