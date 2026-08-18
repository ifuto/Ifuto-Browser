use akl_core::codegen;
use akl_core::bytecode::Runtime;
use akl_core::parser::Parser;

#[test]
fn dbg() {
    let src = "function outer() { var x = 10; function inner() { return x; } return inner(); } outer();";
    let program = Parser::new(src).parse_program().unwrap();
    let mut rt = Runtime::new();
    let fidx = codegen::compile(&mut rt, &program).unwrap();
    for (i, f) in rt.funcs.iter().enumerate() {
        eprintln!("fn[{i}] n_params={} n_locals={} code={:?}", f.n_params, f.n_locals, f.code);
    }
    let r = rt.run(fidx, &[]);
    eprintln!("result={:?}", r);
}
