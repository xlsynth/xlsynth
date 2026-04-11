enum Flavor: u2 {
  Vanilla = 0,
  Mint = 1,
}

sum Choice {
  None,
  FlavorChoice(Flavor),
  Wide(u16),
}

fn main(x: Choice) -> bool {
  x == x
}
