enum SparseChoice : s3 {
  Negative(u8) = -1,
  Positive = 2,
}

fn f(value: SparseChoice) -> u8 {
  let payload = if let SparseChoice::Negative(active) = value {
    active
  } else {
    u8:0
  };
  match value {
    SparseChoice::Negative(_) => payload,
    SparseChoice::Positive => payload,
    invalid!(raw) => raw as u8,
  }
}
