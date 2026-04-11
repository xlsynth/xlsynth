enum Empty: u2 {
}

sum MaybeImpossible {
  Unit,
  Impossible(Empty),
}

fn f(x: MaybeImpossible) -> u32 {
  match x {
    MaybeImpossible::Unit => u32:0,
  }
}
