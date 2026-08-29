fn main() {
    println!(
        "Node          = {}",
        std::mem::size_of::<ifuto_core::dom::Node>()
    );
    println!(
        "NameStr       = {}",
        std::mem::size_of::<ifuto_core::dom::NameStr>()
    );
    println!(
        "Option<NodeId>= {}",
        std::mem::size_of::<Option<ifuto_core::dom::NodeId>>()
    );
    println!(
        "Doctype       = {}",
        std::mem::size_of::<ifuto_core::dom::Doctype>()
    );
    println!(
        "Opt<Doctype>  = {}",
        std::mem::size_of::<Option<ifuto_core::dom::Doctype>>()
    );
    println!(
        "Vec<Attr>     = {}",
        std::mem::size_of::<Vec<ifuto_core::html_tok::Attr>>()
    );
    println!(
        "Attr          = {}",
        std::mem::size_of::<ifuto_core::html_tok::Attr>()
    );
}
