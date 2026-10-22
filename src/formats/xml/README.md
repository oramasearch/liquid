# XML canonicalisation

This private parser accepts well-formed elements, quoted attributes, comments, declarations, and
namespace declarations. It canonicalises expanded names and sorted attributes for OOXML and ODF;
it is not a general XML schema or entity-expansion engine.

Element descent checks the shared 64-level structural limit before parsing the element. Each
element keeps only the namespaces it declares and resolves a prefix by walking outward through
its ancestors, so a document with many declarations and many children never copies them per
child. One document has 1,000,000 work units shared by all elements and attributes; sibling
elements do not reset the allowance. Exhaustion returns an XML error (liquid #434).
