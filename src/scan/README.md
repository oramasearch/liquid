# Secret scanner implementation

The private detector set uses bounded, hand-written token matchers and line-oriented assignment
checks. Detector output contains only an identifier, description, and line number. Raw matched
values never cross this directory's interface.

The JWT matcher consumes maximal base64url runs and keeps two flags for preceding header and
payload components. It examines each run once plus one fixed-prefix search, so repeated `eyJ`
candidates take linear work and constant auxiliary storage. Later components remain eligible as
headers for another token. There is no input truncation or file-size refusal.
